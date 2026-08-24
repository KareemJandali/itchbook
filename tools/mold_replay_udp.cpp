// mold_replay_udp — send a MoldUDP64 packet file over the wire, on a schedule.
//
// mold_wrap puts the framing back on a NASDAQ sample file; mold_replay reads it
// from disk. This sends it through a socket, which is the only way to get a
// thread boundary and therefore the only way to have a latency at all.
//
// THE SCHEDULE IS COMPUTED IN ADVANCE AND NOTHING CHANGES IT.
//
// That is the whole design, and it is a defence against coordinated omission --
// the trap where a load generator that waits for the system under test stops
// sampling exactly when the answer gets interesting. If the book stalls for
// 10 ms, a sender that paces off completions sends nothing during the stall and
// records no slow samples, so its percentile table describes only the periods
// when nothing was wrong. The name is apt: the generator has quietly
// coordinated with the system to omit its worst behaviour.
//
// So the intended send time of message N is fixed before the first packet
// leaves: t0 + N/rate. The sender never reads the receiver, never sleeps on a
// completion, and when it falls behind it does NOT slow down -- it sends late
// and records how late.
//
// WHICH MAKES ITS OWN LATENESS A RESULT, NOT AN ASIDE. A sender that could not
// keep its schedule has invalidated the run, and the only way to know is to
// measure it. Lateness gets its own histogram, reported separately from
// anything the receiver will produce, and if it is not small compared with the
// latency being reported then the experiment measured the sender.
//
// AND IT DOES NOT DECOMPRESS IN ITS OWN SEND LOOP.
//
// The first version did, and the lateness histogram said so twice over. Once as
// a single ~5.8 ms outlier at every rate -- the schedule started before gzip's
// first inflate, so packet zero was always late by the cost of opening the
// stream, which is the phase-4 slab story in a different coat. And once as a
// tail that grew with the rate, because inflating in the hot loop makes the
// generator the slowest thing in the experiment.
//
// So packets are read into memory first and the clock starts at the first send.
// A load generator that competes with the system under test for CPU is not
// measuring it. The cost is memory -- bounded, reported, and refused above a
// stated limit rather than discovered as a swap storm.
//
// Usage:
//   mold_replay_udp <packets.gz> [--host H] [--port N] [--rate MSG/S]
//                   [--multiplier X] [--limit N] [--json out.json]
//                   [--lateness-csv out.csv] [--quiet]
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sched.h>
#include <sys/mman.h>
#include <string>
#include <vector>

#include "itchbook/bench/histogram.hpp"
#include "itchbook/bench/rdtsc.hpp"
#include "itchbook/itch/messages.hpp"
#include "itchbook/itch/reader.hpp"
#include "itchbook/mold/packet.hpp"

using namespace itchbook;

namespace {

struct Options {
    const char* packets = nullptr;
    std::string host = "127.0.0.1";
    uint16_t port = 26400;
    double rate = 0.0;          // messages per second; 0 = as fast as possible
    double multiplier = 0.0;    // replay the feed's own clock at X speed
    uint64_t limit = 0;         // packets; 0 = all
    const char* json = nullptr;
    const char* lateness_csv = nullptr;
    size_t max_preload_mb = 4096;
    uint64_t spin_margin_ns = 500000;   // 500 us; see wait_until()

    // Real-time scheduling priority for the sending thread. 0 = do not ask.
    //
    // The pacing loop is already accurate to tens of NANOSECONDS when it is
    // running -- measured p50 lateness of 35 ns on a pinned run. Its whole error
    // budget goes to a tail where the thread is not running at all: p99.9 of
    // 142 us and a max of 1.09 ms, which is a scheduler quantum, not a pacing
    // mistake. Spinning cannot help a thread that is off the CPU, and a bigger
    // spin margin cannot either -- the observed tail is already INSIDE the
    // existing 500 us margin.
    //
    // SCHED_FIFO is the standard answer and had never been asked for here.
    int rt_priority = 0;
    bool quiet = false;
};

// What the OS actually gave us, as opposed to what was asked for. Recorded and
// reported rather than assumed: a run that silently failed to get real-time
// priority and one that never asked for it produce the same lateness and must
// not produce the same report.
struct RealTime {
    bool requested = false;
    bool scheduler_granted = false;
    bool memory_locked = false;
    std::string scheduler_error;
    std::string memory_error;
};

// Ask for SCHED_FIFO and a locked address space. Neither is required; both are
// reported. CAP_SYS_NICE is what grants the first (run as root, or once:
// `sudo setcap cap_sys_nice=ep <binary>`), CAP_IPC_LOCK or a raised RLIMIT_MEMLOCK
// the second.
RealTime go_realtime(int priority) {
    RealTime rt;
    if (priority <= 0) return rt;
    rt.requested = true;

    sched_param sp{};
    sp.sched_priority = priority;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0) {
        rt.scheduler_granted = true;
    } else {
        rt.scheduler_error = std::strerror(errno);
    }

    // A major fault in the pacing loop is a millisecond that no amount of
    // priority prevents.
    if (mlockall(MCL_CURRENT | MCL_FUTURE) == 0) {
        rt.memory_locked = true;
    } else {
        rt.memory_error = std::strerror(errno);
    }
    return rt;
}

// Wall clock in nanoseconds. Deliberately NOT the TSC: the schedule spans a
// whole session and has to be comparable with a rate expressed in seconds, and
// this is the one place in the project where a coarse, absolute, monotonic
// clock is the right instrument rather than a cycle counter.
uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Wait until `deadline`, precisely, without burning a core for the whole wait.
//
// nanosleep alone overshoots by tens of microseconds and a pure spin costs a
// core at every rate. So: sleep until a margin short of the deadline, then spin
// the rest. The margin is generous because oversleeping cannot be undone, while
// spinning a little longer than necessary only costs cycles.
void wait_until(uint64_t deadline, uint64_t spin_margin_ns) {
    for (;;) {
        const uint64_t now = now_ns();
        if (now >= deadline) return;
        const uint64_t remaining = deadline - now;
        if (remaining > spin_margin_ns) {
            // Sleep the bulk, spin the margin. How big the margin has to be is
            // a property of the machine's timer, not of this program: measured
            // here, a 100 us margin left a 145 us tail because nanosleep
            // overshot it, and lateness of that size would swamp a loopback
            // wire-to-book figure entirely. It is a knob for that reason.
            timespec ts;
            const uint64_t sleep_ns = remaining - spin_margin_ns;
            ts.tv_sec = static_cast<time_t>(sleep_ns / 1000000000ULL);
            ts.tv_nsec = static_cast<long>(sleep_ns % 1000000000ULL);
            nanosleep(&ts, nullptr);
        }
    }
}

// The first ITCH timestamp inside a MoldUDP64 packet, for --multiplier. Returns
// false for a heartbeat or an end-of-session packet, which carry no messages
// and therefore no clock.
bool first_timestamp(const uint8_t* packet, size_t len, uint64_t* out) {
    mold::Header h;
    if (!mold::parse_header(packet, len, &h)) return false;
    if (h.message_count() == 0) return false;
    mold::BlockIterator it(packet, len, h.message_count());
    const uint8_t* msg = nullptr;
    uint16_t msg_len = 0;
    if (!it.next(&msg, &msg_len) || msg_len < 11) return false;
    *out = itch::timestamp(msg);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--host") opt.host = next("--host");
        else if (a == "--port") opt.port = static_cast<uint16_t>(std::atoi(next("--port")));
        else if (a == "--rate") opt.rate = std::atof(next("--rate"));
        else if (a == "--multiplier") opt.multiplier = std::atof(next("--multiplier"));
        else if (a == "--limit") opt.limit = std::strtoull(next("--limit"), nullptr, 10);
        else if (a == "--json") opt.json = next("--json");
        else if (a == "--lateness-csv") opt.lateness_csv = next("--lateness-csv");
        else if (a == "--spin-margin-us") {
            opt.spin_margin_ns = std::strtoull(next("--spin-margin-us"), nullptr, 10) * 1000;
        }
        else if (a == "--max-preload-mb") {
            opt.max_preload_mb = static_cast<size_t>(std::strtoull(next("--max-preload-mb"),
                                                                   nullptr, 10));
        }
        else if (a == "--rt-priority") opt.rt_priority = std::atoi(next("--rt-priority"));
        else if (a == "--quiet") opt.quiet = true;
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "error: unknown option %s\n", a.c_str());
            return 2;
        } else if (opt.packets == nullptr) {
            opt.packets = argv[i];
        } else {
            std::fprintf(stderr, "error: unexpected argument %s\n", a.c_str());
            return 2;
        }
    }
    if (opt.packets == nullptr) {
        std::fprintf(stderr,
                     "usage: %s <packets.gz> [--host H] [--port N] [--rate MSG/S]\n"
                     "       [--multiplier X] [--limit N] [--json out.json]\n"
                     "       [--lateness-csv out.csv] [--rt-priority N] [--quiet]\n",
                     argv[0]);
        return 2;
    }
    if (opt.rate > 0.0 && opt.multiplier > 0.0) {
        std::fprintf(stderr, "error: --rate and --multiplier are two different schedules;"
                             " pick one\n");
        return 2;
    }

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "error: socket: %s\n", std::strerror(errno));
        return 1;
    }
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(opt.port);
    if (::inet_pton(AF_INET, opt.host.c_str(), &dst.sin_addr) != 1) {
        std::fprintf(stderr, "error: bad host %s\n", opt.host.c_str());
        ::close(fd);
        return 1;
    }

    uint64_t send_end = 0;
    bench::Histogram lateness(1 << 20);
    uint64_t packets = 0;
    uint64_t messages = 0;
    uint64_t send_errors = 0;
    uint64_t bytes = 0;
    uint64_t feed_t0 = 0;
    bool feed_t0_known = false;
    // What the sender ACTUALLY managed, as distinct from what it was told to
    // manage. Those are the same number only while it keeps its schedule, and
    // a sweep that quotes the offered rate as the sustained rate is quoting an
    // argument it passed to itself. The pipeline can only have absorbed what
    // was really sent.
    double achieved = 0.0;
    double send_seconds = 0.0;

    // ---- load first, send second -------------------------------------------
    std::vector<std::vector<uint8_t>> wire;
    uint64_t preload_ns = 0;
    try {
        const uint64_t t0 = now_ns();
        Reader reader(opt.packets);
        std::vector<uint8_t> buf;
        size_t held = 0;
        const size_t cap = opt.max_preload_mb * 1024 * 1024;
        while (reader.next(buf)) {
            if (opt.limit != 0 && wire.size() >= opt.limit) break;
            held += buf.size() + sizeof(std::vector<uint8_t>);
            if (held > cap) {
                std::fprintf(stderr,
                             "error: %s needs more than %zu MB to preload. Slice the feed, or\n"
                             "       raise --max-preload-mb knowing the generator now competes\n"
                             "       with the system under test for memory bandwidth.\n",
                             opt.packets, opt.max_preload_mb);
                ::close(fd);
                return 1;
            }
            wire.push_back(buf);
        }
        preload_ns = now_ns() - t0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        ::close(fd);
        return 1;
    }

    // AFTER the preload, before the first send. Locking the address space is
    // cheaper once the buffers exist, and raising priority earlier would only
    // give real-time scheduling to a decompression loop.
    const RealTime rt = go_realtime(opt.rt_priority);
    if (rt.requested && !opt.quiet) {
        if (rt.scheduler_granted) {
            std::printf("%-30s SCHED_FIFO priority %d\n", "scheduling", opt.rt_priority);
        } else {
            std::printf("%-30s DENIED (%s) -- run as root, or once:\n"
                        "%-30s   sudo setcap cap_sys_nice=ep <this binary>\n",
                        "SCHED_FIFO", rt.scheduler_error.c_str(), "");
        }
        if (!rt.memory_locked) {
            std::printf("%-30s not locked (%s)\n", "address space",
                        rt.memory_error.c_str());
        }
    }

    {
        // The clock starts at the first send, not before the first read. Any
        // one-time cost that happens before a single packet is on the wire is
        // not lateness, and charging it to packet zero puts a startup artefact
        // in the worst-sample column of every table downstream.
        const uint64_t start = now_ns();
        uint64_t last_intended = start;
        send_end = start;

        for (const std::vector<uint8_t>& buf : wire) {

            // The schedule, computed from the message count SO FAR -- messages,
            // not packets, because a rate in packets per second means nothing
            // when packets hold between one and forty messages.
            uint64_t intended = start;
            if (opt.rate > 0.0) {
                intended = start + static_cast<uint64_t>(
                    static_cast<double>(messages) / opt.rate * 1e9);
            } else if (opt.multiplier > 0.0) {
                uint64_t feed_ts = 0;
                if (first_timestamp(buf.data(), buf.size(), &feed_ts)) {
                    if (!feed_t0_known) {
                        feed_t0 = feed_ts;
                        feed_t0_known = true;
                    }
                    // A feed whose clock goes backwards within a packet stream
                    // would otherwise schedule a send in the past and then
                    // never catch up; clamp rather than reorder.
                    const uint64_t elapsed = feed_ts > feed_t0 ? feed_ts - feed_t0 : 0;
                    intended = start + static_cast<uint64_t>(
                        static_cast<double>(elapsed) / opt.multiplier);
                } else {
                    intended = last_intended;
                }
            }
            last_intended = intended;

            if (opt.rate > 0.0 || opt.multiplier > 0.0) {
                wait_until(intended, opt.spin_margin_ns);
            }

            const uint64_t actual = now_ns();
            const ssize_t n = ::sendto(fd, buf.data(), buf.size(), 0,
                                       reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
            if (n < 0) {
                ++send_errors;
            } else {
                bytes += static_cast<uint64_t>(n);
            }

            // Lateness, not latency. Zero when the sender kept its schedule,
            // and if it is ever comparable with the wire-to-book figures then
            // those figures are about this program.
            lateness.add(actual > intended ? actual - intended : 0);

            mold::Header h;
            send_end = actual;
            if (mold::parse_header(buf.data(), buf.size(), &h)) {
                messages += h.message_count();
            }
            ++packets;
        }
        send_seconds = static_cast<double>(send_end - start) / 1e9;
        if (send_seconds > 0.0) achieved = static_cast<double>(messages) / send_seconds;
    }
    ::close(fd);

    lateness.finalize();
    const double late_p50 = lateness.percentile(50);
    const double late_p99 = lateness.percentile(99);
    const double late_p999 = lateness.percentile(99.9);

    if (!opt.quiet) {
        std::printf("%-30s %s:%u\n", "destination", opt.host.c_str(), opt.port);
        if (opt.rate > 0.0) {
            std::printf("%-30s %.0f msg/s\n", "scheduled rate", opt.rate);
        } else if (opt.multiplier > 0.0) {
            std::printf("%-30s %.2fx the feed's own clock\n", "scheduled rate", opt.multiplier);
        } else {
            std::printf("%-30s %s\n", "scheduled rate", "unpaced — as fast as the socket takes");
        }
        std::printf("%-30s %" PRIu64 " (%.2f s to load, not counted as lateness)\n",
                    "packets preloaded", static_cast<uint64_t>(wire.size()),
                    static_cast<double>(preload_ns) / 1e9);
        std::printf("%-30s %.0f msg/s\n", "achieved rate", achieved);
        std::printf("%-30s %" PRIu64 "\n", "packets sent", packets);
        std::printf("%-30s %" PRIu64 "\n", "messages sent", messages);
        std::printf("%-30s %" PRIu64 "\n", "bytes sent", bytes);
        std::printf("%-30s %" PRIu64 "\n", "send errors", send_errors);
        std::printf("\nsender lateness (actual minus intended), nanoseconds\n");
        std::printf("%-30s %14.0f\n", "p50", late_p50);
        std::printf("%-30s %14.0f\n", "p99", late_p99);
        std::printf("%-30s %14.0f\n", "p99.9", late_p999);
        std::printf("%-30s %14u\n", "worst", lateness.max());
        // The sender is the instrument. An instrument that cannot keep its own
        // schedule is measuring itself, and saying so here is cheaper than
        // discovering it in a latency table three steps later.
        if (opt.rate > 0.0 || opt.multiplier > 0.0) {
            // Ten microseconds, because loopback wire-to-book is measured in
            // microseconds and a sender tail of the same order contaminates it
            // completely. A millisecond -- the first threshold here -- would
            // have passed a run whose sender was two orders of magnitude
            // noisier than the thing it was measuring.
            constexpr double kSenderTailBudgetNs = 10000.0;
            std::printf("\n%s\n", late_p999 < kSenderTailBudgetNs
                ? "The sender kept its schedule: p99.9 lateness is under 10 us."
                : "WARNING: p99.9 sender lateness is above 10 us, which is the same order as\n"
                  "the latency this run exists to measure. Raise --spin-margin-us, lower the\n"
                  "rate, or treat the results as measuring the generator.");
        }
    }

    if (opt.lateness_csv != nullptr && !lateness.write_buckets_csv(opt.lateness_csv)) {
        std::fprintf(stderr, "error: cannot write %s\n", opt.lateness_csv);
        return 1;
    }
    if (opt.json != nullptr) {
        std::FILE* f = std::fopen(opt.json, "w");
        if (f == nullptr) {
            std::fprintf(stderr, "error: cannot write %s\n", opt.json);
            return 1;
        }
        std::fprintf(f,
            "{\n  \"packets\": %" PRIu64 ",\n  \"messages\": %" PRIu64 ",\n"
            "  \"bytes\": %" PRIu64 ",\n  \"send_errors\": %" PRIu64 ",\n"
            "  \"rate_msg_per_s\": %.3f,\n  \"multiplier\": %.3f,\n"
            "  \"achieved_msg_per_s\": %.1f,\n  \"send_seconds\": %.6f,\n"
            "  \"lateness_ns\": {\"p50\": %.0f, \"p99\": %.0f, \"p999\": %.0f, \"max\": %u},\n"
            "  \"realtime\": {\"requested\": %s, \"priority\": %d,"
            " \"scheduler_granted\": %s, \"memory_locked\": %s,"
            " \"scheduler_error\": \"%s\"}\n}\n",
            packets, messages, bytes, send_errors, opt.rate, opt.multiplier,
            achieved, send_seconds,
            late_p50, late_p99, late_p999, lateness.max(),
            rt.requested ? "true" : "false", opt.rt_priority,
            rt.scheduler_granted ? "true" : "false",
            rt.memory_locked ? "true" : "false", rt.scheduler_error.c_str());
        if (std::fclose(f) != 0) return 1;
    }
    return send_errors == 0 ? 0 : 1;
}
