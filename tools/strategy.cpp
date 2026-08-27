//
// strategy.cpp — phase 12.7's strategy process.
//
// Consumes ITCH over UDP into its own book, speaks OUCH over TCP, and quotes.
// A separate OS process from the exchange; every byte between them crosses a
// real socket.
//
// THE CLAUSE THIS FILE EXISTS TO SATISFY. The done-condition asks for the
// strategy's "own fills observed back through its own feed handler, not
// reported to it by the exchange side". In production a strategy learns fills
// authoritatively from OUCH Executed and treats ITCH as market data, so this is
// not a claim about how to build a trading system. It is a claim about which
// code paths actually ran: there must exist an ITCH 'E' naming a reference this
// strategy owns, which left the exchange as a MoldUDP64 datagram, arrived on
// this process's UDP socket, and was applied to a book that already held that
// reference because an ITCH 'A' for it crossed the same socket earlier.
// PUBLISH -> TRANSPORT -> CONSUME, proven end to end.
//
// Two consequences, worth stating rather than discovering later:
//
//   * Only MAKER fills are visible this way. ITCH describes what happens to
//     RESTING orders, so a strategy order that crosses is never named on the
//     tape. "It traded" is therefore not enough; the gate requires maker fills
//     specifically, and this file counts them separately from everything else.
//   * OUCH remains load-bearing for IDENTITY. The only way this process learns
//     which ITCH reference belongs to its token is the Accepted message's
//     reference number. The clause forbids the fill EVENT arriving over OUCH,
//     not the token-to-reference binding, and pretending otherwise would
//     misstate what was demonstrated.
//
// The fill detector reads the book BEFORE the message is applied. An execution
// that fills an order completely destroys it, so a detector looking afterwards
// finds nothing and reports zero fills -- the exact shape of a vacuous pass.
//
// WHY IT QUOTES AT THE BID RATHER THAN INSIDE IT. split.hpp's aggressor walks
// the named order's own price level from the front, and fills strategy orders
// it meets BEFORE reaching the named one. A strategy order is therefore filled
// only when a HISTORICAL order at the same price, added AFTER it, is executed.
// Joining the current best bid is what makes that possible: the level churns,
// later adds land behind us, and one of them eventually trades. Improving the
// bid by a tick would create a level no historical order ever joins, so nothing
// could ever be executed there and the fill count would be a guaranteed zero.
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
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "itchbook/book/book_set.hpp"
#include "itchbook/book/dispatch.hpp"
#include "itchbook/itch/messages.hpp"
#include "itchbook/mold/packet.hpp"
#include "itchbook/mold/sequencer.hpp"
#include "itchbook/ouch/encode.hpp"
#include "itchbook/ouch/messages.hpp"
#include "itchbook/replay/split.hpp"
#include "itchbook/soupbin/session.hpp"

namespace {

namespace sb = itchbook::soupbin;
namespace bk = itchbook::book;
namespace ouch = itchbook::ouch;
namespace m = itchbook::itch;

constexpr uint64_t kTickPeriodNs = 100'000'000;

uint64_t wall_now_ns() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

struct Opt {
    std::string symbol = "TEST";
    uint16_t tcp_port = 27001;
    uint16_t udp_port = 27002;
    uint32_t quote_shares = 100;
    int32_t tick = 100;               // 1c in ITCH's 1/10000-dollar prices
    int32_t quote_offset_ticks = 2;   // see the banner: 0 gets picked off
    uint64_t quote_every = 200;       // feed messages between quotes
    uint64_t max_orders = 200;
    bool drop_ouch_executed = false;  // the positive arm
    uint64_t ack_delay_ms = 0;        // hold Accepted this long; see the banner
    std::string json;
    uint64_t trace = 0;
};

struct OutQueue : sb::Sink {
    std::vector<uint8_t> buf;
    void on_message(const uint8_t* p, size_t n) override { buf.insert(buf.end(), p, p + n); }
};

// Everything this process believes, built only from bytes that arrived on a
// socket. Nothing here is shared with the exchange -- it is a different address
// space, and that is the point.
// What the feed told us about a reference before we were told it was ours.
// See the banner: the ack is not ordered against the datagrams.
struct Parked {
    struct Exec { uint32_t shares; bool in_book; char side; };
    bool saw_add = false;
    std::vector<Exec> execs;
};

struct Belief {
    bk::BookSet set{1u << 22, 100, 20, 512};
    std::unordered_set<uint64_t> my_refs;      // from OUCH Accepted
    std::unordered_map<uint64_t, Parked> parked;
    uint64_t applied = 0;
    uint64_t own_adds_seen = 0;
    uint64_t adds_before_ack = 0;   // the feed beat the acknowledgement
    uint64_t execs_before_ack = 0, execs_before_ack_shares = 0;
    uint64_t maker_fills = 0, maker_fill_shares = 0;
    uint64_t maker_fills_in_book = 0;   // ...and it was resting in OUR book
    int64_t position = 0;
    uint16_t locate = 0;                       // learned from the feed
    bool have_locate = false;

    // The Accepted for `ref` has just landed. Anything the feed already said
    // about it counts now, exactly as it would have counted then.
    void retire(uint64_t ref) {
        const auto it = parked.find(ref);
        if (it == parked.end()) return;
        if (it->second.saw_add) ++own_adds_seen;
        for (const Parked::Exec& e : it->second.execs) {
            ++maker_fills;
            maker_fill_shares += e.shares;
            if (e.in_book) {
                ++maker_fills_in_book;
                position += (e.side == 'B') ? int64_t(e.shares) : -int64_t(e.shares);
            }
        }
        parked.erase(it);
    }
};

// The Sequencer's Handler. It is where PUBLISH -> TRANSPORT -> CONSUME lands:
// on_message() is only ever reached by a byte that came off the UDP socket and
// survived MoldUDP64 sequencing.
struct FeedHandler {
    Belief* b = nullptr;
    char want[8] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};

    // ITCH stock fields are 8 bytes, space-padded, never NUL-terminated.
    bool same_symbol(const uint8_t* stock) const {
        return std::memcmp(stock, want, 8) == 0;
    }

    void on_message(char type, const uint8_t* p, uint16_t len) {
        // Which book is ours. Taking the locate from the first Add of any
        // symbol is right only by accident on a single-symbol feed; on a real
        // one it is a different stock's book, and every quote after it is
        // priced off the wrong touch.
        if (!b->have_locate) {
            if (type == 'R' && len >= 39 &&
                same_symbol(m::stock_directory::stock(p))) {
                b->locate = m::stock_locate(p);
                b->have_locate = true;
            } else if ((type == 'A' || type == 'F') && len >= 36 &&
                       same_symbol(m::add_order::stock(p))) {
                b->locate = m::stock_locate(p);
                b->have_locate = true;
            }
        }

        // ---- the fill detector, BEFORE the book mutates ----------------------
        //
        // The order is load-bearing, not stylistic: book::apply is about to
        // destroy any order this execution fills completely, so the same lookup
        // a few lines later would find nothing and report zero fills.
        if (type == 'E' && len >= 31) {
            const uint64_t ref = m::order_executed::ref(p);
            if (b->my_refs.count(ref) == 0 && itchbook::replay::is_strategy_ref(ref)) {
                // A fill for an order in the strategy partition whose Accepted
                // has not crossed TCP yet. Park everything the feed knows right
                // now -- including whether it was resting in our book, which
                // cannot be asked again once book::apply has run -- and retire
                // it when the ack lands. Dropping it silently lost 202 shares.
                const uint32_t sh = m::order_executed::executed_shares(p);
                const bk::Book* mb = b->have_locate ? b->set.peek(b->locate) : nullptr;
                const bk::Order* po = mb != nullptr ? mb->find(ref) : nullptr;
                b->parked[ref].execs.push_back(
                    {sh, po != nullptr,
                     po != nullptr ? static_cast<char>(po->side) : ' '});
                ++b->execs_before_ack;
                b->execs_before_ack_shares += sh;
            }
            if (b->my_refs.count(ref) != 0) {
                const uint32_t sh = m::order_executed::executed_shares(p);
                ++b->maker_fills;
                b->maker_fill_shares += sh;
                // Was it actually in the book we built from the feed? A
                // reference number coming back proves only that a number came
                // back; this proves the 'A' that carried it was received,
                // parsed and applied.
                const bk::Book* mine = b->have_locate ? b->set.peek(b->locate) : nullptr;
                const bk::Order* o = mine != nullptr ? mine->find(ref) : nullptr;
                if (o != nullptr) {
                    ++b->maker_fills_in_book;
                    // The side comes from the book, not from a map keyed on
                    // having seen the ack first -- see the banner.
                    b->position += (static_cast<char>(o->side) == 'B')
                                       ? int64_t(sh) : -int64_t(sh);
                }
            }
        }
        if ((type == 'A' || type == 'F') && len >= 36) {
            const uint64_t ref = m::add_order::ref(p);
            if (b->my_refs.count(ref) != 0) {
                ++b->own_adds_seen;
            } else if (itchbook::replay::is_strategy_ref(ref)) {
                // Ours by the feed's own partition, but the Accepted that names
                // it has not arrived yet. Parked, and counted as own_adds_seen
                // when it does.
                b->parked[ref].saw_add = true;
                ++b->adds_before_ack;
            }
        }

        if (bk::apply(b->set, type, p)) ++b->applied;
    }

    // A gap means the book is no longer trustworthy. Recording it is enough
    // here: the gate requires zero, and a run with gaps is not evidence of
    // anything, so it fails rather than being repaired.
    void on_gap(uint64_t, uint64_t) {}
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
        if (a == "--symbol") opt.symbol = next("--symbol");
        else if (a == "--tcp-port") opt.tcp_port = uint16_t(std::atoi(next("--tcp-port")));
        else if (a == "--udp-port") opt.udp_port = uint16_t(std::atoi(next("--udp-port")));
        else if (a == "--quote-shares") opt.quote_shares = uint32_t(std::atoi(next("--quote-shares")));
        else if (a == "--tick") opt.tick = std::atoi(next("--tick"));
        else if (a == "--quote-offset-ticks") opt.quote_offset_ticks = std::atoi(next("--quote-offset-ticks"));
        else if (a == "--quote-every") opt.quote_every = std::strtoull(next("--quote-every"), nullptr, 10);
        else if (a == "--max-orders") opt.max_orders = std::strtoull(next("--max-orders"), nullptr, 10);
        else if (a == "--drop-ouch-executed") opt.drop_ouch_executed = true;
        else if (a == "--ack-delay-ms") opt.ack_delay_ms = std::strtoull(next("--ack-delay-ms"), nullptr, 10);
        else if (a == "--json") opt.json = next("--json");
        else if (a == "--trace") opt.trace = std::strtoull(next("--trace"), nullptr, 10);
        else { std::fprintf(stderr, "error: unknown option %s\n", a.c_str()); return 2; }
    }

    // ---- UDP feed socket, bound BEFORE announcing READY -------------------------
    const int ufd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (ufd < 0) { std::perror("socket udp"); return 1; }
    int one = 1;
    ::setsockopt(ufd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    // Ask for 8MB and then find out what was granted. Linux clamps to
    // net.core.rmem_max and says nothing: on this machine the request becomes
    // 416 KB, which is what actually stands between an unpaced feed and a
    // dropped datagram. A number nobody reads back is an assumption.
    int rcvbuf = 8 << 20;
    ::setsockopt(ufd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    int rcvbuf_granted = 0;
    socklen_t rcvbuf_len = sizeof(rcvbuf_granted);
    if (::getsockopt(ufd, SOL_SOCKET, SO_RCVBUF, &rcvbuf_granted, &rcvbuf_len) != 0) {
        rcvbuf_granted = 0;
    }
    sockaddr_in ua{};
    ua.sin_family = AF_INET;
    ua.sin_port = htons(opt.udp_port);
    ua.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(ufd, reinterpret_cast<sockaddr*>(&ua), sizeof(ua)) != 0) {
        std::fprintf(stderr, "error: bind udp %u: %s\n", opt.udp_port, std::strerror(errno));
        return 1;
    }
    set_nonblock(ufd);

    // ---- TCP to the exchange ----------------------------------------------------
    const int tcp = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in ta{};
    ta.sin_family = AF_INET;
    ta.sin_port = htons(opt.tcp_port);
    ::inet_pton(AF_INET, "127.0.0.1", &ta.sin_addr);
    if (::connect(tcp, reinterpret_cast<sockaddr*>(&ta), sizeof(ta)) != 0) {
        std::fprintf(stderr, "error: connect %u: %s\n", opt.tcp_port, std::strerror(errno));
        return 1;
    }
    int nod = 1;
    ::setsockopt(tcp, IPPROTO_TCP, TCP_NODELAY, &nod, sizeof(nod));
    set_nonblock(tcp);

    // tick() is a deadline rather than a descriptor: soupbin/session.hpp needs
    // it to fire independently of socket readability, and an unconditional test
    // at the top of the loop cannot be skipped by any path through it. poll()
    // rather than epoll because epoll is Linux-only and there are two fds.
    uint64_t next_tick_ns = wall_now_ns() + kTickPeriodNs;

    Belief belief;
    FeedHandler handler;
    handler.b = &belief;
    std::memcpy(handler.want, opt.symbol.data(),
                opt.symbol.size() > 8 ? 8 : opt.symbol.size());
    itchbook::mold::Sequencer<FeedHandler> seq;
    OutQueue out;

    uint64_t ouch_executed_received = 0, ouch_executed_shares = 0;
    uint64_t datagrams = 0, orders_sent = 0, accepted = 0, rejected = 0, canceled = 0;

    sb::ClientSession::Config ccfg;
    ccfg.server_dead_threshold_ns = 30ULL * 1'000'000'000ULL;
    ccfg.login_response_timeout_ns = 10ULL * 1'000'000'000ULL;

    // Inbound OUCH. Accepted binds a token to a reference -- the only way this
    // process can recognise its own orders on the tape. Executed is COUNTED and
    // never used to move the position: the positive arm drops these entirely
    // and the run must still observe fills.
    struct OuchIn : sb::Sink {
        Belief* belief = nullptr;
        std::unordered_set<uint64_t>* refs = nullptr;
        uint64_t* accepted = nullptr;
        uint64_t* rejected = nullptr;
        uint64_t* canceled = nullptr;
        uint64_t* execs = nullptr;
        uint64_t* exec_shares = nullptr;
        bool drop_executed = false;
        uint64_t delay_ns = 0;
        uint64_t now = 0;                                  // set before on_bytes
        std::deque<std::pair<uint64_t, uint64_t>>* late = nullptr;   // due_ns, ref
        void on_message(const uint8_t* p, size_t n) override {
            if (n < 1) return;
            switch (static_cast<char>(p[0])) {
                case 'A':
                    if (n >= ouch::kAcceptedLen) {
                        const uint64_t r = ouch::accepted::reference_number(p);
                        if (delay_ns != 0 && late != nullptr) {
                            late->emplace_back(now + delay_ns, r);
                        } else {
                            refs->insert(r);
                            // Anything the feed already said about r counts now.
                            if (belief != nullptr) belief->retire(r);
                        }
                        ++*accepted;
                    }
                    break;
                case 'J': ++*rejected; break;
                case 'C': ++*canceled; break;
                case 'E':
                    if (n >= ouch::kExecutedLen && !drop_executed) {
                        ++*execs;
                        *exec_shares += ouch::executed::executed_shares(p);
                    }
                    break;
                default: break;
            }
        }
    } ouch_in;
    ouch_in.belief = &belief;
    ouch_in.refs = &belief.my_refs;
    ouch_in.accepted = &accepted;
    ouch_in.rejected = &rejected;
    ouch_in.canceled = &canceled;
    ouch_in.execs = &ouch_executed_received;
    ouch_in.exec_shares = &ouch_executed_shares;
    ouch_in.drop_executed = opt.drop_ouch_executed;
    std::deque<std::pair<uint64_t, uint64_t>> late_acks;
    ouch_in.delay_ns = opt.ack_delay_ms * 1'000'000ULL;
    ouch_in.late = &late_acks;

    const uint64_t wall_start = wall_now_ns();
    sb::ClientSession client(ccfg, wall_start, "STRAT", "PASSWORD", "", "1", &out, &ouch_in);

    // See the exchange: a write to a socket whose peer has gone would kill
    // this process before it could report anything. Reproduced in both
    // directions at wait status 141.
    ::signal(SIGPIPE, SIG_IGN);

    std::printf("READY strategy udp=%u tcp=%u\n", opt.udp_port, opt.tcp_port);
    std::fflush(stdout);

    std::vector<uint8_t> dg(65536);
    uint64_t next_token = 1;
    uint64_t last_quote_at = 0;
    uint64_t ended_at = 0;
    bool exchange_gone = false;

    while (true) {
        int timeout_ms = 20;
        const uint64_t before = wall_now_ns();
        const int to_tick = next_tick_ns > before
            ? static_cast<int>((next_tick_ns - before) / 1'000'000ULL) : 0;
        if (to_tick < timeout_ms) timeout_ms = to_tick;

        // TCP first, then UDP: learn who you are, then read the tape. It
        // does not close the ack race -- the ack may simply arrive later -- but
        // it stops the loop from manufacturing one by reading a datagram it
        // already held the answer to.
        pollfd pfd[2];
        pfd[0].fd = tcp; pfd[0].events = POLLIN; pfd[0].revents = 0;
        pfd[1].fd = ufd; pfd[1].events = POLLIN; pfd[1].revents = 0;
        const int n = ::poll(pfd, 2, timeout_ms);
        const uint64_t wall = wall_now_ns();

        // Unconditional, not a branch of the poll result.
        if (wall >= next_tick_ns) {
            next_tick_ns = wall + kTickPeriodNs;
            client.tick(wall);
        }

        // Acknowledgements that were held now come due, in the order they
        // arrived. Everything the feed already said about each reference is
        // retired here, which is the path --ack-delay-ms exists to exercise.
        while (!late_acks.empty() && late_acks.front().first <= wall) {
            const uint64_t r = late_acks.front().second;
            late_acks.pop_front();
            belief.my_refs.insert(r);
            belief.retire(r);
        }

        for (int k = 0; n > 0 && k < 2; ++k) {
            if (pfd[k].revents == 0) continue;
            if (pfd[k].fd == tcp) {
                uint8_t rb[8192];
                for (;;) {
                    const ssize_t r = ::read(tcp, rb, sizeof(rb));
                    if (r <= 0) break;
                    ouch_in.now = wall;
                    client.on_bytes(rb, size_t(r), wall);
                }
            } else if (pfd[k].fd == ufd) {
                for (;;) {
                    const ssize_t r = ::recvfrom(ufd, dg.data(), dg.size(), 0, nullptr, nullptr);
                    if (r <= 0) break;
                    ++datagrams;
                    seq.on_packet(dg.data(), size_t(r), handler);
                }
            }
        }

        // ---- quote -------------------------------------------------------------
        //
        // Rest a buy BEHIND the best bid. At the bid it is marketable by the
        // time it lands, because the book it was priced from is however many
        // messages stale -- measured, not assumed: see the banner. A maker fill
        // is the only kind the feed can ever show, so an order that crosses is
        // an order this phase cannot observe.
        if (client.state() == sb::State::LoggedIn && orders_sent < opt.max_orders &&
            belief.have_locate && belief.applied - last_quote_at >= opt.quote_every) {
            const bk::Book* b = belief.set.peek(belief.locate);
            int32_t bid = 0;
            int32_t ask = 0;
            int32_t base = 0;
            if (b != nullptr && b->best_bid(&bid)) {
                base = bid;
                // Cap: never bid at or above the best offer. On a coherent book
                // this is a no-op; on a crossed one it is the difference
                // between resting and crossing. See the banner.
                if (b->best_ask(&ask) && ask - opt.tick < base) base = ask - opt.tick;
            }
            const int32_t px = base > 0 ? base - opt.quote_offset_ticks * opt.tick : 0;
            if (px > 0) {
                last_quote_at = belief.applied;
                char tok[15];
                std::snprintf(tok, sizeof(tok), "S%013" PRIu64, next_token++);
                uint8_t msg[ouch::kEnterOrderLen];
                const size_t mn = ouch::encode::enter_order(
                    msg, tok, 'B', opt.quote_shares, opt.symbol.c_str(), px, 0,
                    "STRT", 'Y', 'A', 'N', 0, 'N', ' ');
                client.send_unsequenced(msg, mn, wall);
                ++orders_sent;
                if (orders_sent <= opt.trace) {
                    std::printf("quote %-3" PRIu64 " after %8" PRIu64 " msgs  bid=%d ask=%d px=%d\n",
                                orders_sent, belief.applied, bid, ask, px);
                    std::fflush(stdout);
                }
            }
        }

        if (!out.buf.empty()) {
            const ssize_t w = ::write(tcp, out.buf.data(), out.buf.size());
            if (w > 0) out.buf.erase(out.buf.begin(), out.buf.begin() + w);
            else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                     errno != EINTR) {
                // The exchange has gone. Stop quoting and let the run finish
                // reporting rather than spinning on a broken pipe.
                out.buf.clear();
                exchange_gone = true;
            }
        }

        // The feed's end-of-session is the exchange saying it is finished. Give
        // the TCP side a moment to deliver the last acks before leaving.
        if (seq.ended()) {
            if (ended_at == 0) {
                ended_at = wall;
                // Say goodbye. The exchange finishes when the session is
                // terminal, and a client that simply exits leaves it waiting
                // out a 60-second timeout on every single run.
                client.send_logout(wall);
            }
            if (wall - ended_at > 500'000'000ULL && out.buf.empty()) break;
        }
        if (exchange_gone && seq.ended()) break;
        if (wall - wall_start > 180ULL * 1'000'000'000ULL) break;
    }

    // Anything still held would be an ack the run simply never applied, and
    // its fills would go missing for a reason that has nothing to do with the
    // transports. Drain before reporting.
    while (!late_acks.empty()) {
        const uint64_t r = late_acks.front().second;
        late_acks.pop_front();
        belief.my_refs.insert(r);
        belief.retire(r);
    }

    seq.flush(handler);
    const auto& st = seq.stats();

    std::printf("\n=== strategy ===\n");
    std::printf("%-32s %14" PRIu64 "\n", "datagrams received", datagrams);
    std::printf("%-32s %14d\n", "UDP rcvbuf actually granted", rcvbuf_granted);
    std::printf("%-32s %14" PRIu64 "\n", "feed messages applied", belief.applied);
    std::printf("%-32s %14" PRIu64 "\n", "sequencer gaps", st.gaps);
    std::printf("%-32s %14" PRIu64 "\n", "messages lost", st.messages_lost);
    std::printf("%-32s %14" PRIu64 "\n", "orders sent", orders_sent);
    std::printf("%-32s %14" PRIu64 "\n", "OUCH accepted", accepted);
    std::printf("%-32s %14" PRIu64 "\n", "OUCH rejected", rejected);
    std::printf("%-32s %14" PRIu64 "\n", "OUCH canceled", canceled);
    std::printf("%-32s %14" PRIu64 "\n", "own adds seen ON THE FEED", belief.own_adds_seen);
    std::printf("%-32s %14" PRIu64 "\n", "  feed beat the ack", belief.adds_before_ack);
    std::printf("%-32s %14" PRIu64 "\n", "FILLS THAT BEAT THE ACK", belief.execs_before_ack);
    std::printf("%-32s %14" PRIu64 "\n", "  shares", belief.execs_before_ack_shares);
    std::printf("%-32s %14" PRIu64 "\n", "MAKER FILLS FROM THE FEED", belief.maker_fills);
    std::printf("%-32s %14" PRIu64 "\n", "  resting in MY OWN book", belief.maker_fills_in_book);
    std::printf("%-32s %14" PRIu64 "\n", "  shares", belief.maker_fill_shares);
    std::printf("%-32s %14" PRIu64 "\n", "OUCH Executed received", ouch_executed_received);
    std::printf("%-32s %14" PRIu64 "\n", "  shares", ouch_executed_shares);
    std::printf("%-32s %14" PRId64 "\n", "position, feed-derived only", belief.position);

    if (!opt.json.empty()) {
        std::FILE* j = std::fopen(opt.json.c_str(), "w");
        if (j != nullptr) {
            std::fprintf(j,
                "{\n  \"datagrams\": %" PRIu64 ",\n  \"rcvbuf_granted\": %d,\n  \"feed_messages\": %" PRIu64 ",\n"
                "  \"gaps\": %" PRIu64 ",\n  \"messages_lost\": %" PRIu64 ",\n"
                "  \"orders_sent\": %" PRIu64 ",\n  \"ouch_accepted\": %" PRIu64 ",\n"
                "  \"ouch_rejected\": %" PRIu64 ",\n  \"ouch_canceled\": %" PRIu64 ",\n"
                "  \"own_adds_seen_on_feed\": %" PRIu64 ",\n  \"adds_before_ack\": %" PRIu64 ",\n  \"execs_before_ack\": %" PRIu64 ",\n  \"execs_before_ack_shares\": %" PRIu64 ",\n"
                "  \"maker_fills_from_feed\": %" PRIu64 ",\n  \"maker_fills_in_my_book\": %" PRIu64 ",\n  \"maker_fill_shares\": %" PRIu64 ",\n"
                "  \"ouch_executed_received\": %" PRIu64 ",\n  \"ouch_executed_shares\": %" PRIu64 ",\n"
                "  \"position_from_feed\": %" PRId64 "\n}\n",
                datagrams, rcvbuf_granted, belief.applied, st.gaps, st.messages_lost,
                orders_sent, accepted, rejected, canceled, belief.own_adds_seen,
                belief.adds_before_ack, belief.execs_before_ack,
                belief.execs_before_ack_shares,
                belief.maker_fills, belief.maker_fills_in_book, belief.maker_fill_shares,
                ouch_executed_received, ouch_executed_shares, belief.position);
            std::fclose(j);
        }
    }
    return 0;
}
