// cpu_jitter — can this machine hold a CPU?
//
// Phase 10's entrance exam has two halves. mold_replay_udp answers the first:
// can a load generator keep a schedule? This answers the one underneath it,
// which is prior to every latency number in the phase and which nothing in the
// repository measured: when a thread is pinned to a CPU and asks for nothing
// else, does it keep running?
//
// THE METHOD IS A LOOP WITH NOTHING IN IT. Read a monotonic clock, subtract the
// previous reading, record the difference. On a machine that leaves the thread
// alone that difference is the cost of the read -- tens of nanoseconds, flat.
// Every value materially above it is time the thread was NOT EXECUTING, because
// nothing in the loop can take longer. No sampling, no instrumentation, no
// attribution problem: the gaps are the answer.
//
// WHY THIS EXISTS SEPARATELY FROM tsc_offset. That tool asks whether two cores'
// clocks agree, which is a question about the clock. This asks whether a core
// is available, which is a question about the scheduler -- and on a virtualised
// host, about a scheduler the guest cannot see. Both can pass while the other
// fails, and phase 10 needs both: the wire-to-book sample is a subtraction
// between two clocks (tsc_offset) taken on two threads that have to be running
// when the messages arrive (this).
//
// AND IT REPORTS WHAT THE KERNEL THINKS, BESIDE WHAT HAPPENED. That comparison
// is the whole point on a guest. CLOCK_THREAD_CPUTIME_ID and the involuntary
// context-switch count come from the same kernel that scheduled the thread, so
// when the loop observes hundreds of milliseconds inside gaps while the kernel
// credits it with the entire wall clock and reports a handful of preemptions,
// the time was taken BELOW the kernel -- by a hypervisor -- and no in-guest
// setting reaches it. A guest that is losing the CPU without being told cannot
// diagnose itself from /proc/pressure or from steal time, both of which read
// zero in exactly this case. This is the difference between "the scheduler
// preempted us", which isolcpus and SCHED_FIFO can fix, and "the vCPU was
// descheduled", which they cannot.
//
// RUN SEVERAL AT ONCE TO TELL THOSE APART. --cpus takes a list and starts one
// thread per CPU from the same barrier. If the gaps land at the same instants
// with the same durations on every CPU, the machine is being taken away
// wholesale; if they interleave, they are per-CPU scheduling. One run answers a
// question that a hundred single-CPU runs cannot.
//
// Usage:
//   cpu_jitter [--cpus 13,14,15] [--seconds 15] [--json out.json]
#include <pthread.h>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#endif

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Options {
    std::vector<int> cpus;
    double seconds = 15.0;
    const char* json = nullptr;
};

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t thread_cpu_ns() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// One gap worth keeping whole. The percentiles come from a sorted vector rather
// than a histogram because the interesting values are the rare large ones, and a
// histogram's top bucket is exactly where it stops being able to tell you the
// number -- which is the mistake this phase already made once, reading bucket
// EDGES as if they were samples.
struct Gap {
    uint64_t at_ns;      // when it started, relative to the run
    uint64_t len_ns;
};

struct Result {
    int cpu = -1;
    bool pinned = false;
    uint64_t samples = 0;
    uint64_t wall_ns = 0;
    uint64_t cpu_ns = 0;
    uint64_t p50 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
    uint64_t over_10us = 0, over_100us = 0, over_1ms = 0, over_10ms = 0;
    uint64_t ns_in_gaps_over_100us = 0;
    long vol_ctxsw = 0, invol_ctxsw = 0;
    std::vector<Gap> worst;      // the ten largest, with timestamps
};

// -1 means "this platform does not tell you", which is not zero. A context
// switch count of 0 and a count that could not be read are different claims,
// and the second one must not be able to masquerade as the first.
long read_status_long(const char* key) {
#if !defined(__linux__)
    (void)key;
    return -1;
#else
    std::FILE* f = std::fopen("/proc/thread-self/status", "r");
    if (f == nullptr) return -1;
    char line[256];
    long v = -1;
    const size_t klen = std::strlen(key);
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::strncmp(line, key, klen) == 0) {
            v = std::strtol(line + klen, nullptr, 10);
            break;
        }
    }
    std::fclose(f);
    return v;
#endif
}

void spin(int cpu, double seconds, std::atomic<bool>* go, Result* out) {
    out->cpu = cpu;
    // LINUX ONLY, AND THE NON-LINUX PATH REFUSES RATHER THAN QUIETLY SUCCEEDING
    // -- the same rule mold_replay_udp's go_realtime() follows, and for the same
    // reason. macOS has no thread-affinity API, so a thread there is wherever
    // the scheduler put it this microsecond. Reporting `pinned` true on such a
    // platform would put gap counts in a table under a heading they do not
    // belong to: this tool's whole claim is "one thread, one CPU, nothing else
    // asked of it", and without the pinning it measures a thread wandering
    // between cores instead. The verdict below already refuses when `pinned` is
    // false on any CPU, so the refusal costs nothing extra here.
    //
    // Unguarded, this cost a macOS CI build -- which is exactly the job that
    // exists to catch Linux-only calls compiling green on every runner that
    // matters until they do not.
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    out->pinned = pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    out->pinned = false;
#endif

    // Reserve before the clock starts. A reallocation inside the loop would be
    // recorded as a gap, and it would be this program's gap.
    std::vector<Gap> gaps;
    gaps.reserve(1u << 20);

    while (!go->load(std::memory_order_acquire)) { }

    const uint64_t t0 = now_ns();
    const uint64_t c0 = thread_cpu_ns();
    const uint64_t deadline = t0 + static_cast<uint64_t>(seconds * 1e9);
    uint64_t prev = t0;
    uint64_t n = 0;
    for (;;) {
        const uint64_t t = now_ns();
        const uint64_t d = t - prev;
        ++n;
        // 2 us is far above the cost of the read and far below anything that
        // matters, so this keeps every gap that could possibly be a stall while
        // keeping the vector bounded on a healthy machine.
        if (d > 2000 && gaps.size() < gaps.capacity()) {
            gaps.push_back({prev - t0, d});
        }
        prev = t;
        if (t >= deadline) break;
    }
    out->wall_ns = prev - t0;
    out->cpu_ns = thread_cpu_ns() - c0;
    out->samples = n;
    out->vol_ctxsw = read_status_long("voluntary_ctxt_switches:");
    out->invol_ctxsw = read_status_long("nonvoluntary_ctxt_switches:");

    for (const Gap& g : gaps) {
        if (g.len_ns > 10000) ++out->over_10us;
        if (g.len_ns > 100000) { ++out->over_100us; out->ns_in_gaps_over_100us += g.len_ns; }
        if (g.len_ns > 1000000) ++out->over_1ms;
        if (g.len_ns > 10000000) ++out->over_10ms;
    }

    // PERCENTILES OVER SAMPLES ARE THE WRONG INSTRUMENT HERE, and the first
    // version of this tool used them and drew the wrong conclusion from them.
    //
    // The loop takes ~1.28 BILLION samples in 20 seconds, so 99.99% of them are
    // the clock read and nothing else. Every percentile up to p99.99 therefore
    // reads zero even on a machine losing 11 ms at a time -- and a verdict that
    // gated on "p99.9 under 10 us" duly announced that this host holds a CPU
    // while its own max column said 11,448,396 ns. The percentile was not
    // wrong; it was answering "what does a typical clock read cost", which
    // nobody asked.
    //
    // What the question actually needs is HOW OFTEN and HOW LONG: a thread that
    // must respond inside 10 us cares about the number of times per second it
    // was away for longer than that, and about the worst one. Those are counts,
    // not quantiles. The percentiles are still computed and reported, clearly
    // labelled as being over the recorded gaps rather than over the samples,
    // because they describe the shape of the tail once you are in it.
    std::vector<uint64_t> lens;
    lens.reserve(gaps.size());
    for (const Gap& g : gaps) lens.push_back(g.len_ns);
    std::sort(lens.begin(), lens.end());
    auto pct = [&](double p) -> uint64_t {
        if (lens.empty()) return 0;
        size_t i = static_cast<size_t>(p / 100.0 * static_cast<double>(lens.size()));
        if (i >= lens.size()) i = lens.size() - 1;
        return lens[i];
    };
    out->p50 = pct(50); out->p99 = pct(99); out->p999 = pct(99.9);
    out->p9999 = pct(99.99);
    out->max = lens.empty() ? 0 : lens.back();

    std::sort(gaps.begin(), gaps.end(),
              [](const Gap& a, const Gap& b) { return a.len_ns > b.len_ns; });
    for (size_t i = 0; i < gaps.size() && i < 10; ++i) out->worst.push_back(gaps[i]);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "error: %s needs a value\n", flag); std::exit(2); }
            return argv[++i];
        };
        if (a == "--cpus") {
            std::string s = next("--cpus");
            size_t p = 0;
            while (p < s.size()) {
                size_t c = s.find(',', p);
                if (c == std::string::npos) c = s.size();
                opt.cpus.push_back(std::atoi(s.substr(p, c - p).c_str()));
                p = c + 1;
            }
        } else if (a == "--seconds") opt.seconds = std::atof(next("--seconds"));
        else if (a == "--json") opt.json = next("--json");
        else { std::fprintf(stderr, "error: unknown option %s\n", a.c_str()); return 2; }
    }
    if (opt.cpus.empty()) {
        std::fprintf(stderr, "usage: %s --cpus 13,14,15 [--seconds 15] [--json out.json]\n", argv[0]);
        return 2;
    }

    std::vector<Result> results(opt.cpus.size());
    std::vector<std::thread> threads;
    std::atomic<bool> go{false};
    for (size_t i = 0; i < opt.cpus.size(); ++i) {
        threads.emplace_back(spin, opt.cpus[i], opt.seconds, &go, &results[i]);
    }
    // The barrier matters: gaps that coincide across CPUs are the finding, and
    // staggered starts would smear it.
    go.store(true, std::memory_order_release);
    for (std::thread& t : threads) t.join();

    std::printf("%-5s %-7s %9s %9s %9s %12s %9s %9s %8s %8s %9s\n",
                "cpu", "pinned", "/s>10us", "/s>100us", "/s>1ms", "max ns",
                "gap p50", "gap p99", "gap max", "lost %", "unacct ms");
    for (const Result& r : results) {
        const double unacct = (static_cast<double>(r.wall_ns) - static_cast<double>(r.cpu_ns)) / 1e6;
        const double secs = static_cast<double>(r.wall_ns) / 1e9;
        const double lost = r.wall_ns ? 100.0 * static_cast<double>(r.ns_in_gaps_over_100us) /
                                        static_cast<double>(r.wall_ns) : 0.0;
        std::printf("%-5d %-7s %9.0f %9.0f %9.1f %12" PRIu64 " %9" PRIu64 " %9" PRIu64
                    " %8" PRIu64 " %8.2f %9.3f\n",
                    r.cpu, r.pinned ? "yes" : "NO",
                    secs > 0 ? r.over_10us / secs : 0.0,
                    secs > 0 ? r.over_100us / secs : 0.0,
                    secs > 0 ? r.over_1ms / secs : 0.0,
                    r.max, r.p50, r.p99, r.p999, lost, unacct);
    }
    std::printf("\nA gap is time the thread was NOT RUNNING: nothing in the loop can take\n"
                "longer than a clock read. The first three columns are gaps per second over\n"
                "each threshold and they are what decides this -- gap p50/p99/max describe\n"
                "the shape of the tail ONCE YOU ARE IN IT, over recorded gaps only, and say\n"
                "nothing about how often you are. 'lost %%' is wall time inside gaps over\n"
                "100 us; 'unacct ms' is wall minus what the kernel credited as CPU.\n");
    for (const Result& r : results) {
        const double secs = static_cast<double>(r.wall_ns) / 1e9;
        std::printf("\ncpu %d: %" PRIu64 " samples over %.3f s -- %.0f gaps/s over 10 us, "
                    "%.2f%% of wall time lost to gaps over 100 us\n",
                    r.cpu, r.samples, secs,
                    secs > 0 ? static_cast<double>(r.over_10us) / secs : 0.0,
                    r.wall_ns ? 100.0 * static_cast<double>(r.ns_in_gaps_over_100us) /
                                static_cast<double>(r.wall_ns) : 0.0);
        std::printf("       kernel says: %.3f ms of CPU against %.3f ms of wall, "
                    "%ld involuntary context switches\n",
                    static_cast<double>(r.cpu_ns) / 1e6,
                    static_cast<double>(r.wall_ns) / 1e6, r.invol_ctxsw);
        std::printf("       ten worst, at t= (s): ");
        for (const Gap& g : r.worst) {
            std::printf("%" PRIu64 "@%.4f ", g.len_ns, static_cast<double>(g.at_ns) / 1e9);
        }
        std::printf("\n");
    }
    // The verdict the phase actually needs, stated rather than left to the
    // reader: a wire-to-book figure in microseconds cannot be taken on a
    // machine whose idle, pinned CPU loses more than that at p99.9.
    uint64_t worst_max = 0, total_over_10us = 0, total_over_100us = 0, worst_p999 = 0;
    double total_secs = 0.0;
    bool all_pinned = true;
    for (const Result& r : results) {
        worst_max = std::max(worst_max, r.max);
        worst_p999 = std::max(worst_p999, r.p999);
        total_over_10us += r.over_10us;
        total_over_100us += r.over_100us;
        total_secs = std::max(total_secs, static_cast<double>(r.wall_ns) / 1e9);
        all_pinned = all_pinned && r.pinned;
    }
    // THE THRESHOLD IS 100 us, NOT ZERO GAPS OVER 10 us, and the first version
    // of this got that wrong in a way worth recording. It demanded LITERALLY
    // ZERO gaps over 10 us -- a bar no real machine clears -- and duly announced
    // that a bare-metal host did "NOT hold a CPU" while its own table showed
    // 30 gaps a second over 10 us, NONE at all over 100 us, and a 43 us worst
    // case. The same run's sender held a 46 ns p99.9 schedule.
    //
    // What the phase actually needs is that nothing interrupts long enough to
    // move a microsecond-scale p99.9. A gap over 100 us lands in the tail of any
    // run this repository takes; gaps in the tens of microseconds at a few tens
    // per second do not -- 30/s across a 60 s sweep is 1,800 samples out of five
    // million, which reaches p99.96 and not p99.9. So: no gaps over 100 us is
    // the bar, and the rate over 10 us is reported beside it as context rather
    // than as a gate.
    //
    // For scale, the two machines this was written against: bare metal gave 30
    // gaps/s over 10 us and ZERO over 100 us; the WSL2 guest on the same silicon
    // gave 1,343/s over 10 us, ~90/s over 100 us, and an 11 ms worst case.
    const double rate = total_secs > 0
        ? static_cast<double>(total_over_10us) / total_secs / static_cast<double>(results.size())
        : 0.0;
    if (!all_pinned) {
        std::printf("\nVERDICT: could not pin. Nothing here describes a CPU.\n");
    } else if (total_over_100us == 0) {
        std::printf("\nVERDICT: this machine holds a CPU. Not one gap over 100 us on any CPU\n"
                    "tried, across %.0f s each -- %.0f per second over 10 us, worst %.0f us.\n"
                    "Nothing here is long enough to move a microsecond-scale p99.9.\n",
                    total_secs, rate, static_cast<double>(worst_max) / 1e3);
    } else {
        std::printf("\nVERDICT: this machine does NOT hold a CPU. An idle, pinned CPU with\n"
                    "nothing else asked of it was off-CPU for longer than 100 us about %.0f\n"
                    "times per second, worst %.2f ms. A latency measured here in microseconds\n"
                    "is measuring the scheduler underneath it.\n",
                    total_secs > 0 ? total_over_100us / total_secs / results.size() : 0.0,
                    static_cast<double>(worst_max) / 1e6);
    }

    if (opt.json != nullptr) {
        std::FILE* f = std::fopen(opt.json, "w");
        if (f == nullptr) { std::fprintf(stderr, "error: cannot write %s\n", opt.json); return 1; }
        std::fprintf(f, "{\n  \"seconds\": %.3f,\n  \"all_pinned\": %s,\n"
                        "  \"worst_p999_ns\": %" PRIu64 ",\n  \"worst_max_ns\": %" PRIu64 ",\n"
                        "  \"holds_a_cpu\": %s,\n  \"gaps_over_10us_per_cpu_per_second\": %.1f,\n"
                        "  \"cpus\": [\n",
                     opt.seconds, all_pinned ? "true" : "false", worst_p999, worst_max,
                     (all_pinned && total_over_100us == 0) ? "true" : "false", rate);
        for (size_t i = 0; i < results.size(); ++i) {
            const Result& r = results[i];
            std::fprintf(f,
                "    {\"cpu\": %d, \"pinned\": %s, \"samples\": %" PRIu64 ", "
                "\"wall_ns\": %" PRIu64 ", \"thread_cpu_ns\": %" PRIu64 ", "
                "\"unaccounted_ns\": %" PRId64 ", "
                "\"involuntary_ctxsw\": %ld, \"voluntary_ctxsw\": %ld, "
                "\"gap_ns\": {\"p50\": %" PRIu64 ", \"p99\": %" PRIu64 ", "
                "\"p999\": %" PRIu64 ", \"p9999\": %" PRIu64 ", \"max\": %" PRIu64 "}, "
                "\"gaps_over\": {\"10us\": %" PRIu64 ", \"100us\": %" PRIu64 ", "
                "\"1ms\": %" PRIu64 ", \"10ms\": %" PRIu64 "}, "
                "\"ns_in_gaps_over_100us\": %" PRIu64 ", \"worst\": [",
                r.cpu, r.pinned ? "true" : "false", r.samples, r.wall_ns, r.cpu_ns,
                static_cast<int64_t>(r.wall_ns) - static_cast<int64_t>(r.cpu_ns),
                r.invol_ctxsw, r.vol_ctxsw,
                r.p50, r.p99, r.p999, r.p9999, r.max,
                r.over_10us, r.over_100us, r.over_1ms, r.over_10ms,
                r.ns_in_gaps_over_100us);
            for (size_t j = 0; j < r.worst.size(); ++j) {
                std::fprintf(f, "%s{\"at_ns\": %" PRIu64 ", \"len_ns\": %" PRIu64 "}",
                             j ? ", " : "", r.worst[j].at_ns, r.worst[j].len_ns);
            }
            std::fprintf(f, "]}%s\n", i + 1 < results.size() ? "," : "");
        }
        std::fprintf(f, "  ]\n}\n");
        if (std::fclose(f) != 0) return 1;
    }
    return 0;
}
