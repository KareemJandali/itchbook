//
// exchange.cpp — phase 12.7's exchange process.
//
// One process holding the replayer, the matcher, the gateway and the ITCH
// publisher, accepting OUCH over TCP and publishing ITCH over UDP.
//
// TWO PROCESSES, NOT THREE, AND THE PLAN SAID THREE. The build plan describes
// "three processes: the historical-state replayer of 12.1, the exchange, and
// the strategy". Those are three ROLES and only two can be address spaces. The
// replayer calls Matcher::apply_external_fill() synchronously inside the
// aggressor's queue walk, and split.hpp's own comment says why the return
// value cannot be fire-and-forget: it is "the single source of truth for how
// much moved", so that a Meta/book desync surfaces as a wrong quantity rather
// than propagating. That is a synchronous call on a hot path, and the book it
// walks is pointer-linked, so shared memory would mean rewriting pool.hpp and
// level.hpp for offset-based links. docs/phase12-design.md section 4 requires
// one book per symbol; a socket between the replayer and the matcher would
// contradict the topology the same plan binds this run to. The sockets the
// done-condition actually names are between the STRATEGY and the exchange, and
// those are real.
//
// THE EVENT LOOP IS SINGLE-THREADED, AND tick() IS NOT A BRANCH OF THE poll().
// soupbin/session.hpp is explicit that tick() must come from an independent
// periodic source and not from socket readability, because a peer that has gone
// silent never makes a socket readable again. A bare wait-timeout branch is the
// same bug wearing a different hat: under a busy feed the loop returns early on
// the replay deadline every iteration and the timeout branch never runs, so
// tick() ends up gated behind activity after all -- just the feed's activity
// instead of the peer's.
//
// So the tick is an unconditional deadline test at the top of every iteration,
// against the same monotonic clock it is about, with the poll timeout clamped
// so the loop cannot sleep past it. That is stronger than the timerfd this
// replaced, which still needed its descriptor to be reported ready; no path
// through this loop can skip it. poll() rather than epoll because epoll is
// Linux-only, it broke the macOS build, and its advantage is O(1) readiness
// over thousands of descriptors where there are two.
//
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "itchbook/book/book_set.hpp"
#include "itchbook/emit/sink.hpp"
#include "itchbook/engine/gateway.hpp"
#include "itchbook/itch/reader.hpp"
#include "itchbook/mold/publisher.hpp"
#include "itchbook/replay/split.hpp"
#include "itchbook/soupbin/session.hpp"

namespace {

namespace eng = itchbook::engine;
namespace sb = itchbook::soupbin;
namespace bk = itchbook::book;
namespace rp = itchbook::replay;

constexpr uint16_t kLocate = 1;
constexpr uint64_t kTickPeriodNs = 100'000'000;   // 100ms; see the banner

uint64_t wall_now_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

struct Opt {
    std::string feed;
    std::string symbol = "TEST";
    uint16_t locate = kLocate;   // 1 for every generated feed; MSFT is 5291
    uint16_t tcp_port = 27001;
    uint16_t udp_port = 27002;
    double multiplier = 0.0;        // 0 = as fast as possible
    uint64_t limit = 0;
    size_t mtu = 1400;
    uint64_t heartbeat_ms = 250;
    bool suppress_strategy_exec_itch = false;
    bool wait_for_client = false;
    uint64_t client_timeout_s = 30;   // the negative arm
    std::string json;
    uint64_t trace = 0;
};

// ---- the UDP publish socket ---------------------------------------------------

struct UdpOut {
    int fd = -1;
    sockaddr_in dst{};
    uint64_t datagrams = 0, bytes = 0;
};

void udp_send(void* ctx, const uint8_t* p, size_t n) {
    UdpOut* u = static_cast<UdpOut*>(ctx);
    const ssize_t s = ::sendto(u->fd, p, n, 0,
                               reinterpret_cast<sockaddr*>(&u->dst), sizeof(u->dst));
    if (s > 0) { ++u->datagrams; u->bytes += static_cast<uint64_t>(s); }
}

// Everything the gateway publishes, and everything the replayer publishes,
// goes through one Publisher -- a subscriber sees ONE feed, which is the
// property the 12.6 differential established and this process must preserve.
class FeedSink : public itchbook::emit::Sink {
public:
    explicit FeedSink(itchbook::mold::Publisher* pub) : pub_(pub) {}
    void on_message(const uint8_t* p, size_t n) override {
        if (suppress_strategy_exec && n >= 11 && static_cast<char>(p[0]) == 'E') {
            // The negative arm: drop ITCH executions naming a strategy
            // reference, and nothing else. If the strategy still reports fills
            // with these suppressed, they did not come from the feed.
            uint64_t ref = 0;
            for (int i = 0; i < 8; ++i) ref = (ref << 8) | p[11 + i];
            if (rp::is_strategy_ref(ref)) { ++suppressed; return; }
        }
        pub_->add(p, n);
        ++published;
    }
    itchbook::mold::Publisher* pub_;
    bool suppress_strategy_exec = false;
    uint64_t published = 0, suppressed = 0;
};

// Inbound OUCH: the session hands an Unsequenced Data payload here, and this
// hands it to the gateway. Separate from the gateway because ServerSession
// needs its app_in at construction while the gateway needs the session -- so
// this is built first, with a null gateway, and pointed at it after.
//
// Constructing the session with a null app_in compiles and silently drops
// every inbound order: the exchange runs, publishes a feed, and never trades.
struct OuchIn : sb::Sink {
    eng::Gateway* gw = nullptr;
    bk::Book* book = nullptr;
    uint64_t wall = 0;
    uint64_t trace = 0;
    uint64_t delivered = 0, refused = 0;
    void on_message(const uint8_t* p, size_t n) override {
        if (gw == nullptr) return;
        ++delivered;
        // The book AS THE ORDER ARRIVES -- the thing the strategy could not
        // see, and the only way to tell a stale quote from a mispriced one.
        if (delivered <= trace && n >= itchbook::ouch::kEnterOrderLen &&
            static_cast<char>(p[0]) == 'O' && book != nullptr) {
            int32_t bid = 0, ask = 0;
            const bool hb = book->best_bid(&bid), ha = book->best_ask(&ask);
            std::printf("order %-3" PRIu64 " px=%d   exchange book bid=%s ask=%s\n",
                        delivered,
                        static_cast<int32_t>(itchbook::ouch::enter_order::price(p)),
                        hb ? std::to_string(bid).c_str() : "none",
                        ha ? std::to_string(ask).c_str() : "none");
            std::fflush(stdout);
        }
        if (!gw->on_ouch(p, n, wall)) ++refused;
    }
};

// The gateway's outbound OUCH bytes are queued, never written from inside a
// session dispatch: a partial write would have to be re-entered and both
// session classes assert against reentrancy.
struct OutQueue : sb::Sink {
    std::vector<uint8_t> buf;
    void on_message(const uint8_t* p, size_t n) override { buf.insert(buf.end(), p, p + n); }
};

int set_nonblock(int fd) {
    const int fl = ::fcntl(fd, F_GETFL, 0);
    return ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

}  // namespace

int main(int argc, char** argv) {
    Opt opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* w) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "error: %s needs a value\n", w); std::exit(2); }
            return argv[++i];
        };
        if (a == "--feed") opt.feed = next("--feed");
        else if (a == "--symbol") opt.symbol = next("--symbol");
        else if (a == "--locate") opt.locate = uint16_t(std::atoi(next("--locate")));
        else if (a == "--tcp-port") opt.tcp_port = uint16_t(std::atoi(next("--tcp-port")));
        else if (a == "--udp-port") opt.udp_port = uint16_t(std::atoi(next("--udp-port")));
        else if (a == "--multiplier") opt.multiplier = std::atof(next("--multiplier"));
        else if (a == "--limit") opt.limit = std::strtoull(next("--limit"), nullptr, 10);
        else if (a == "--mtu") opt.mtu = size_t(std::atoi(next("--mtu")));
        else if (a == "--heartbeat-ms") opt.heartbeat_ms = std::strtoull(next("--heartbeat-ms"), nullptr, 10);
        else if (a == "--suppress-strategy-exec-itch") opt.suppress_strategy_exec_itch = true;
        else if (a == "--wait-for-client") opt.wait_for_client = true;
        else if (a == "--client-timeout-s") opt.client_timeout_s = std::strtoull(next("--client-timeout-s"), nullptr, 10);
        else if (a == "--json") opt.json = next("--json");
        else if (a == "--trace") opt.trace = std::strtoull(next("--trace"), nullptr, 10);
        else { std::fprintf(stderr, "error: unknown option %s\n", a.c_str()); return 2; }
    }
    if (opt.feed.empty()) {
        std::fprintf(stderr, "usage: %s --feed <itch.gz> [--tcp-port N] [--udp-port N]\n"
                             "       [--multiplier X] [--limit N] [--json out.json]\n"
                             "       [--suppress-strategy-exec-itch]\n", argv[0]);
        return 2;
    }

    // ---- sockets ---------------------------------------------------------------
    UdpOut udp;
    udp.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp.fd < 0) { std::perror("socket udp"); return 1; }
    udp.dst.sin_family = AF_INET;
    udp.dst.sin_port = htons(opt.udp_port);
    ::inet_pton(AF_INET, "127.0.0.1", &udp.dst.sin_addr);

    const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) { std::perror("socket tcp"); return 1; }
    int one = 1;
    ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in la{};
    la.sin_family = AF_INET;
    la.sin_port = htons(opt.tcp_port);
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(lfd, reinterpret_cast<sockaddr*>(&la), sizeof(la)) != 0) {
        std::fprintf(stderr, "error: bind %u: %s\n", opt.tcp_port, std::strerror(errno));
        return 1;
    }
    if (::listen(lfd, 4) != 0) { std::perror("listen"); return 1; }
    set_nonblock(lfd);

    uint64_t next_tick_ns = wall_now_ns() + kTickPeriodNs;

    // ---- the exchange ----------------------------------------------------------
    bk::BookSet set{1u << 22, 100, 20, 512};
    bk::Book& book = set.at(opt.locate);
    eng::Matcher matcher{book, 100};
    eng::RefSource refs;
    itchbook::risk::KillSwitch kill;

    itchbook::mold::Publisher pub{"CLOSEDLOOP", opt.mtu, &udp_send, &udp};
    FeedSink feed{&pub};
    feed.suppress_strategy_exec = opt.suppress_strategy_exec_itch;

    rp::SplitReplayer replayer{set};
    replayer.set_matcher(&matcher);
    replayer.set_sink(&feed);

    OutQueue out;
    OuchIn ouch_in;
    // Owned here, borrowed everywhere else. The raw pointers are what the hot
    // path reads, and a null one is a meaningful state -- no client yet.
    std::unique_ptr<sb::ServerSession> session_own;
    std::unique_ptr<eng::Gateway> gw_own;
    sb::ServerSession* session = nullptr;
    eng::Gateway* gw = nullptr;
    int cfd = -1;

    eng::Gateway::Config gcfg;
    std::memcpy(gcfg.stock, "        ", 8);
    std::memcpy(gcfg.stock, opt.symbol.data(),
                opt.symbol.size() > 8 ? 8 : opt.symbol.size());

    // READY only after every allocation above. Phase 10 measured a 91-102ms
    // window between a port appearing and a process actually being ready, and
    // paid for it in multi-millisecond latency samples at every rate; the
    // harness waits for this line, not for the port.
    // A write to a socket whose peer has gone raises SIGPIPE, whose default
    // disposition kills the process -- taking the summary and the JSON with it,
    // on exactly the runs whose counters would have said what went wrong.
    // Reproduced at wait status 141 under the sanitised build before this line
    // existed. SIG_IGN is POSIX; MSG_NOSIGNAL is Linux-only and SO_NOSIGPIPE is
    // Darwin-only, and this file has to build on both.
    ::signal(SIGPIPE, SIG_IGN);

    std::printf("READY exchange tcp=%u udp=%u\n", opt.tcp_port, opt.udp_port);
    std::fflush(stdout);

    itchbook::Reader reader(opt.feed);
    std::vector<uint8_t> msg;
    bool feed_done = false;
    bool client_ready = false;
    bool peer_gone = false;
    uint64_t read = 0, applied = 0;
    uint64_t first_replay_ts = 0, last_replay_ts = 0;
    uint64_t feed_done_ns = 0;
    uint64_t wall_start = wall_now_ns();

    while (true) {
        // The replayer is a deadline, not an fd.
        int timeout_ms = 0;
        if (feed_done) timeout_ms = 20;
        else if (opt.wait_for_client && !client_ready) timeout_ms = 20;
        else if (opt.multiplier > 0.0 && first_replay_ts != 0) {
            const uint64_t elapsed_replay =
                static_cast<uint64_t>(double(wall_now_ns() - wall_start) * opt.multiplier);
            const uint64_t due = first_replay_ts + elapsed_replay;
            timeout_ms = last_replay_ts > due ? 1 : 0;
        }
        // ...and never sleep past the tick.
        const uint64_t before = wall_now_ns();
        const int to_tick = next_tick_ns > before
            ? static_cast<int>((next_tick_ns - before) / 1'000'000ULL) : 0;
        if (to_tick < timeout_ms) timeout_ms = to_tick;

        pollfd pfd[2];
        nfds_t nfd = 0;
        pfd[nfd].fd = lfd;  pfd[nfd].events = POLLIN; pfd[nfd].revents = 0; ++nfd;
        if (cfd >= 0) {
            pfd[nfd].fd = cfd; pfd[nfd].events = POLLIN; pfd[nfd].revents = 0; ++nfd;
        }
        const int n = ::poll(pfd, nfd, timeout_ms);
        const uint64_t wall = wall_now_ns();

        // Unconditional, not a branch of the poll result -- see the banner.
        if (wall >= next_tick_ns) {
            next_tick_ns = wall + kTickPeriodNs;
            if (session != nullptr) { session->tick(wall); gw->poll(wall); }
        }

        for (nfds_t k = 0; n > 0 && k < nfd; ++k) {
            if (pfd[k].revents == 0) continue;
            if (pfd[k].fd == lfd) {
                const int c = ::accept(lfd, nullptr, nullptr);
                if (c >= 0) {
                    set_nonblock(c);
                    int nod = 1;
                    ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nod, sizeof(nod));
                    cfd = c;
                    session_own = std::make_unique<sb::ServerSession>(
                        sb::ServerSession::Config{}, wall, &out, &ouch_in);
                    session = session_own.get();
                    gw_own = std::make_unique<eng::Gateway>(gcfg, *session, matcher,
                                                            refs, kill);
                    gw = gw_own.get();
                    gw->set_itch_sink(&feed);
                    ouch_in.gw = gw;
                    ouch_in.book = &book;
                    ouch_in.trace = opt.trace;
                    // Nothing to register: the poll set is rebuilt from cfd
                    // every iteration, so the new client is watched from the
                    // next one.
                }
            } else if (pfd[k].fd == cfd && session != nullptr) {
                uint8_t rb[8192];
                for (;;) {
                    const ssize_t r = ::read(cfd, rb, sizeof(rb));
                    if (r == 0) {
                        // EOF, not EAGAIN. Treating them alike left cfd open
                        // forever against a peer that had gone, so the loop
                        // never reached its own exit condition and kept writing
                        // into a broken pipe.
                        peer_gone = true;
                        break;
                    }
                    if (r > 0) {
                        ouch_in.wall = wall;
                        session->on_bytes(rb, size_t(r), wall);
                        if (session->state() == sb::State::LoginReceived) {
                            gw->decide_login(true, "CLOSEDLOOP", "1", wall);
                        }
                        gw->poll(wall);
                    } else break;
                }
            }
        }

        // Nothing moves until somebody is listening. Without this the whole
        // feed is published to an unbound address while the strategy is still
        // connecting, and the strategy's zeroes measure process startup order
        // rather than the engine.
        if (opt.wait_for_client && !client_ready) {
            if (session != nullptr && session->state() == sb::State::LoggedIn) {
                client_ready = true;
                // The replay clock starts when the day does, not when the
                // process did: otherwise the wait is charged to the multiplier
                // and the first seconds of the feed arrive in a burst.
                wall_start = wall_now_ns();
            } else if (wall_now_ns() - wall_start > opt.client_timeout_s * 1'000'000'000ULL) {
                std::fprintf(stderr,
                    "error: --wait-for-client and no client logged in within %" PRIu64 "s\n",
                    opt.client_timeout_s);
                return 4;
            } else {
                continue;
            }
        }

        // Replay one message if its deadline has come.
        if (!feed_done) {
            bool due = true;
            if (opt.multiplier > 0.0 && first_replay_ts != 0) {
                const uint64_t elapsed_replay =
                    static_cast<uint64_t>(double(wall_now_ns() - wall_start) * opt.multiplier);
                due = last_replay_ts <= first_replay_ts + elapsed_replay;
            }
            if (due) {
                if (reader.next(msg) && (opt.limit == 0 || read < opt.limit)) {
                    ++read;
                    const uint8_t* p = msg.data();
                    const char type = static_cast<char>(p[0]);
                    uint64_t ts = 0;
                    for (int i = 0; i < 6; ++i) ts = (ts << 8) | p[5 + i];
                    if (ts != 0) {
                        if (first_replay_ts == 0) first_replay_ts = ts;
                        last_replay_ts = ts;
                        if (gw != nullptr) gw->set_replay_now(ts);
                    }
                    replayer.apply(type, p);
                    matcher.pump_stops();
                    if (gw != nullptr) gw->pump_fills(wall);
                    ++applied;
                } else {
                    feed_done = true;
                    pub.flush();
                    pub.end_of_session();
                }
            }
        }

        pub.maybe_heartbeat(wall, opt.heartbeat_ms * 1'000'000ULL);

        // Drain the outbound OUCH queue. A failed write is not retried
        // forever against a dead peer: with SIGPIPE ignored it returns EPIPE,
        // and the run should end reporting its counters rather than spinning.
        if (cfd >= 0 && !out.buf.empty()) {
            const ssize_t w = ::write(cfd, out.buf.data(), out.buf.size());
            if (w > 0) out.buf.erase(out.buf.begin(), out.buf.begin() + w);
            else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                     errno != EINTR) {
                peer_gone = true;
            }
        }
        if (peer_gone && cfd >= 0) {
            ::close(cfd);
            cfd = -1;
            out.buf.clear();
        }

        // Done when the feed is exhausted, everything is flushed, and the
        // client has gone away.
        if (feed_done && out.buf.empty() && (cfd < 0 || session == nullptr ||
                                              sb::is_terminal(session->state()))) {
            break;
        }
        // Measured from feed_done, not from the start: wall_start is rebased
        // to the client's login by --wait-for-client, so a replay longer than
        // the window had already spent it before the drain began. The 62-second
        // paced real-day run is over that line.
        if (feed_done) {
            if (feed_done_ns == 0) feed_done_ns = wall_now_ns();
            if (wall_now_ns() - feed_done_ns > 60ULL * 1'000'000'000ULL) break;
        }
    }

    const uint64_t wall_end = wall_now_ns();
    const auto& rc = replayer.counters();

    std::printf("\n=== exchange ===\n");
    std::printf("%-30s %16" PRIu64 "\n", "messages read", read);
    std::printf("%-30s %16" PRIu64 "\n", "applied", applied);
    std::printf("%-30s %16" PRIu64 "\n", "aggressors", rc.aggressors);
    std::printf("%-30s %16" PRIu64 "\n", "strategy shares taken", rc.strategy_shares_taken);
    std::printf("%-30s %16" PRIu64 "\n", "partition violations", rc.partition_violations);
    std::printf("%-30s %16" PRIu64 "\n", "ITCH published", feed.published);
    std::printf("%-30s %16" PRIu64 "\n", "ITCH suppressed (neg arm)", feed.suppressed);
    std::printf("%-30s %16" PRIu64 "\n", "mold packets", pub.packets());
    std::printf("%-30s %16" PRIu64 "\n", "mold heartbeats", pub.heartbeats());
    std::printf("%-30s %16" PRIu64 "\n", "datagrams sent", udp.datagrams);
    if (gw != nullptr) {
        std::printf("%-30s %16" PRIu64 "\n", "OUCH accepted", gw->accepts_sent());
        std::printf("%-30s %16" PRIu64 "\n", "OUCH executed", gw->executes_sent());
        std::printf("%-30s %16" PRIu64 "\n", "OUCH rejected", gw->rejects_sent());
        std::printf("%-30s %16" PRId64 "\n", "gateway position", gw->position());
        std::printf("%-30s %16" PRIu64 "\n", "live orders", gw->live_orders());
    }
    std::printf("%-30s %16" PRIu64 "\n", "OUCH inbound delivered", ouch_in.delivered);
    std::printf("%-30s %16" PRIu64 "\n", "OUCH inbound refused", ouch_in.refused);
    if (false) {
    }
    std::printf("%-30s %16d\n", "conserves_shares", int(matcher.conserves_shares()));
    std::printf("%-30s %16d\n", "agrees_with_book", int(matcher.agrees_with_book()));

    if (!opt.json.empty()) {
        std::FILE* j = std::fopen(opt.json.c_str(), "w");
        if (j != nullptr) {
            std::fprintf(j,
                "{\n  \"messages_read\": %" PRIu64 ",\n  \"applied\": %" PRIu64 ",\n"
                "  \"aggressors\": %" PRIu64 ",\n  \"strategy_shares_taken\": %" PRIu64 ",\n"
                "  \"partition_violations\": %" PRIu64 ",\n  \"itch_published\": %" PRIu64 ",\n"
                "  \"itch_suppressed\": %" PRIu64 ",\n  \"mold_packets\": %" PRIu64 ",\n  \"mold_heartbeats\": %" PRIu64 ",\n"
                "  \"datagrams_sent\": %" PRIu64 ",\n  \"ouch_accepted\": %" PRIu64 ",\n"
                "  \"ouch_executed\": %" PRIu64 ",\n  \"gateway_position\": %" PRId64 ",\n"
                "  \"conserves_shares\": %s,\n  \"agrees_with_book\": %s,\n"
                "  \"wall_ns\": %" PRIu64 "\n}\n",
                read, applied, rc.aggressors, rc.strategy_shares_taken,
                rc.partition_violations, feed.published, feed.suppressed,
                pub.packets(), pub.heartbeats(), udp.datagrams,
                gw ? gw->accepts_sent() : 0, gw ? gw->executes_sent() : 0,
                gw ? gw->position() : 0,
                matcher.conserves_shares() ? "true" : "false",
                matcher.agrees_with_book() ? "true" : "false",
                wall_end - wall_start);
            std::fclose(j);
        }
    }

    if (rc.partition_violations != 0) return 3;
    if (!matcher.conserves_shares() || !matcher.agrees_with_book()) return 3;
    return 0;
}
