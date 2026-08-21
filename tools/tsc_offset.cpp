// tsc_offset — do two cores agree what time it is?
//
// Phase 10's headline sample is (book applied) minus (packet arrived), and
// those two timestamps are taken on different threads pinned to different
// cores. That is the first time this repository has ever subtracted one core's
// clock from another's, and the plan of record asserted for two revisions that
// it was safe because "it is one clock".
//
// It is not one clock. The TSC is per-core. tsc_is_invariant() checks
// constant_tsc and nonstop_tsc -- that the counter ticks at a fixed rate
// regardless of frequency and C-state -- and says nothing whatever about
// whether two cores started counting at the same instant. A constant offset
// between them lands directly and invisibly in every reported latency: positive
// and it inflates the whole distribution, negative and the histogram fills with
// impossible values that read like a clock bug rather than what they are.
//
// So this measures it, and the measurement is a bound rather than a number.
//
// THE METHOD. Two threads ping-pong a token. A stamps t1 and hands off; B
// stamps t2 and hands back; A stamps t3. Whatever instant B's stamp really
// corresponds to lies somewhere in [t1, t3] on A's clock, and the best estimate
// of it is the midpoint. So
//
//     offset  ~=  t2 - (t1 + t3)/2         and     uncertainty <= (t3 - t1)/2
//
// The uncertainty is the round trip, so the tightest sample is the fastest one:
// this reports the estimate from the minimum round trip rather than an average
// over samples that were interrupted. It then swaps the roles and does it
// again, because a real offset changes sign when you measure it backwards and
// an artefact of the protocol does not.
//
// ONE STAMPING INSTRUCTION, NOT TWO. All three timestamps use cycles_end()
// (rdtscp then lfence), including the first, where cycles_begin() would read
// more naturally. rdtscp waits for prior instructions to RETIRE and rdtsc does
// not, so mixing them puts t1 systematically earlier within its own instruction
// stream than t3 is within its -- which drags the midpoint later and makes t2
// look early by a fixed amount, in both directions at once.
//
// That is not a hypothesis. The first version of this tool mixed them and
// reported -37.5 ns forward and -38.9 ns reversed: same sign, same magnitude,
// which is the signature of a protocol artefact and not of two clocks. The sign
// check below caught it on the first run, in the tool built to catch exactly
// that, before any latency table existed to be quietly wrong.
//
// WHAT IT REPORTS RATHER THAN ASSUMES. Which clock the build actually compiled
// in -- on a platform without rdtsc, bench/rdtsc.hpp falls back to
// clock_gettime, which is system-wide, and the cross-core question does not
// arise. Whether pinning was possible, because on a machine that cannot pin
// there is no guarantee the two threads were ever on different cores and a
// small answer means nothing.
//
//   tsc_offset [--samples N] [--cpu-a N] [--cpu-b N] [--json out.json]
//              [--max-offset-ns N]
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

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "itchbook/bench/rdtsc.hpp"

namespace {

// Returns true if this thread is now pinned to `cpu`. False means the platform
// has no thread-affinity call we can make -- macOS does not expose one that
// binds a thread to a core -- and the caller must say so rather than implying a
// control it did not have.
bool pin_to(int cpu) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

struct Result {
    int64_t offset_cycles = 0;      // B's clock minus A's, from the tightest sample
    uint64_t bound_cycles = 0;      // half the fastest round trip
    uint64_t min_round_trip = 0;
    uint64_t median_round_trip = 0;
};

// One direction: `a` stamps, hands to `b`, `b` stamps, hands back.
Result ping_pong(int cpu_a, int cpu_b, uint64_t samples, bool* pinned_a, bool* pinned_b) {
    std::atomic<uint32_t> turn{0};
    std::atomic<bool> ready{false};
    std::vector<uint64_t> t2s(samples);
    std::vector<uint64_t> t1s(samples);
    std::vector<uint64_t> t3s(samples);

    std::thread b([&] {
        *pinned_b = pin_to(cpu_b);
        ready.store(true, std::memory_order_release);
        for (uint64_t i = 0; i < samples; ++i) {
            while (turn.load(std::memory_order_acquire) != 1) {
            }
            t2s[i] = itchbook::bench::cycles_end();
            turn.store(2, std::memory_order_release);
        }
    });

    *pinned_a = pin_to(cpu_a);
    while (!ready.load(std::memory_order_acquire)) {
    }
    for (uint64_t i = 0; i < samples; ++i) {
        t1s[i] = itchbook::bench::cycles_end();   // see the note on one stamping instruction
        turn.store(1, std::memory_order_release);
        while (turn.load(std::memory_order_acquire) != 2) {
        }
        t3s[i] = itchbook::bench::cycles_end();
        turn.store(0, std::memory_order_release);
    }
    b.join();

    Result r;
    std::vector<uint64_t> rtts;
    rtts.reserve(samples);
    uint64_t best = UINT64_MAX;
    for (uint64_t i = 0; i < samples; ++i) {
        if (t3s[i] < t1s[i]) continue;                 // a clock that went backwards
        const uint64_t rtt = t3s[i] - t1s[i];
        rtts.push_back(rtt);
        if (rtt < best) {
            best = rtt;
            // Midpoint computed as t1 + rtt/2 rather than (t1 + t3)/2, which
            // would overflow for counters near the top of the range.
            const uint64_t midpoint = t1s[i] + rtt / 2;
            r.offset_cycles = static_cast<int64_t>(t2s[i]) - static_cast<int64_t>(midpoint);
            r.bound_cycles = rtt / 2;
            r.min_round_trip = rtt;
        }
    }
    if (!rtts.empty()) {
        std::sort(rtts.begin(), rtts.end());
        r.median_round_trip = rtts[rtts.size() / 2];
    }
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    uint64_t samples = 200000;
    int cpu_a = 2;
    int cpu_b = 3;
    const char* json_out = nullptr;
    double max_offset_ns = -1.0;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", flag);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--samples") samples = std::strtoull(next("--samples"), nullptr, 10);
        else if (a == "--cpu-a") cpu_a = std::atoi(next("--cpu-a"));
        else if (a == "--cpu-b") cpu_b = std::atoi(next("--cpu-b"));
        else if (a == "--json") json_out = next("--json");
        else if (a == "--max-offset-ns") max_offset_ns = std::atof(next("--max-offset-ns"));
        else {
            std::fprintf(stderr, "usage: %s [--samples N] [--cpu-a N] [--cpu-b N]"
                                 " [--json out.json] [--max-offset-ns N]\n", argv[0]);
            return 2;
        }
    }

    const bool have_tsc = ITCHBOOK_HAVE_RDTSC != 0;
    const bool invariant = itchbook::bench::tsc_is_invariant();
    const double cyc_per_ns = itchbook::bench::calibrate_cycles_per_ns();

    bool pinned_a = false;
    bool pinned_b = false;
    const Result fwd = ping_pong(cpu_a, cpu_b, samples, &pinned_a, &pinned_b);
    bool pinned_c = false;
    bool pinned_d = false;
    const Result rev = ping_pong(cpu_b, cpu_a, samples, &pinned_c, &pinned_d);

    auto ns = [cyc_per_ns](double cycles) { return cycles / cyc_per_ns; };

    std::printf("%-34s %s\n", "timestamp source",
                have_tsc ? "rdtsc / rdtscp (per-core counter)"
                         : "clock_gettime CLOCK_MONOTONIC (system-wide)");
    if (have_tsc) {
        std::printf("%-34s %s\n", "invariant TSC", invariant ? "yes" : "NO — cycle counts are fiction");
    }
    std::printf("%-34s %s\n", "thread pinning",
                (pinned_a && pinned_b) ? "yes"
                                       : "NOT AVAILABLE on this platform");
    std::printf("%-34s %.3f\n", "cycles per nanosecond", cyc_per_ns);
    std::printf("%-34s %" PRIu64 "\n", "samples per direction", samples);
    std::printf("\n%-34s %14s %14s\n", "", "cycles", "ns");
    std::printf("%-34s %14" PRIu64 " %14.1f\n", "fastest round trip A->B->A",
                fwd.min_round_trip, ns(static_cast<double>(fwd.min_round_trip)));
    std::printf("%-34s %14" PRIu64 " %14.1f\n", "median round trip A->B->A",
                fwd.median_round_trip, ns(static_cast<double>(fwd.median_round_trip)));
    std::printf("%-34s %14" PRId64 " %14.1f\n", "offset estimate  B - A",
                fwd.offset_cycles, ns(static_cast<double>(fwd.offset_cycles)));
    std::printf("%-34s %14" PRId64 " %14.1f\n", "offset estimate  A - B (reversed)",
                rev.offset_cycles, ns(static_cast<double>(rev.offset_cycles)));
    std::printf("%-34s %14" PRIu64 " %14.1f\n", "uncertainty (half fastest RTT)",
                fwd.bound_cycles, ns(static_cast<double>(fwd.bound_cycles)));

    // THE VERDICT, and the order of these tests is the argument.
    //
    // The point estimate is not the answer. Half the fastest round trip is a
    // hard bound on how precisely this method can locate B's stamp on A's
    // timeline, and an estimate smaller than its own bound has not been
    // distinguished from zero -- reporting it as an offset would be reporting
    // the noise floor as a finding.
    //
    // Only if the estimate clears its bound is the sign check worth consulting:
    // a real offset reverses when the roles do, and a protocol asymmetry does
    // not.
    const bool signs_oppose = (fwd.offset_cycles > 0) != (rev.offset_cycles > 0);
    const double worst_ns = ns(static_cast<double>(
        std::max<int64_t>(std::abs(fwd.offset_cycles), std::abs(rev.offset_cycles))));
    const double bound_ns = ns(static_cast<double>(fwd.bound_cycles));
    const bool resolvable = worst_ns > bound_ns;

    std::printf("\n%-34s %14.1f ns\n", "largest |offset| estimate", worst_ns);
    std::printf("%-34s %14.1f ns\n", "resolution of this method", bound_ns);
    std::printf("\n");
    if (!resolvable) {
        std::printf("VERDICT: not distinguishable from zero.\n"
                    "  The estimate is smaller than the method's own resolution, so what is\n"
                    "  bounded is the offset, not measured: it is under %.0f ns. Any latency\n"
                    "  large against that figure may subtract these two clocks; quote the\n"
                    "  bound beside it.\n", bound_ns);
    } else if (!signs_oppose) {
        std::printf("VERDICT: an asymmetry in the handoff, not in the clocks.\n"
                    "  A real offset reverses sign when the roles do and this does not, so\n"
                    "  the protocol is contributing %.0f ns of its own. Fix the protocol\n"
                    "  before believing the number.\n", worst_ns);
    } else {
        std::printf("VERDICT: a real offset of about %.0f ns between these cores.\n"
                    "  It reverses with the roles and it clears the method's resolution.\n"
                    "  Do NOT subtract these two clocks for anything of comparable size.\n"
                    "  Use CLOCK_MONOTONIC_RAW for the cross-thread sample and keep the TSC\n"
                    "  for intra-thread work: a correction is a model, a different clock is\n"
                    "  a fact.\n", worst_ns);
    }

    if (!have_tsc) {
        std::printf("\nThis build reads a system-wide clock, so there is no per-core offset to\n"
                    "find and the numbers above bound the measurement noise instead. The\n"
                    "cross-core question arises only where cycles_begin() is rdtsc.\n");
    }
    if (!pinned_a || !pinned_b) {
        std::printf("\nWithout pinning there is no guarantee the two threads ever ran on\n"
                    "different cores, so a small offset here is not evidence of anything.\n"
                    "Treat this as unmeasured on this platform and say so in the results.\n");
    }

    if (json_out != nullptr) {
        std::FILE* f = std::fopen(json_out, "w");
        if (f == nullptr) {
            std::fprintf(stderr, "error: cannot write %s\n", json_out);
            return 1;
        }
        std::fprintf(f,
            "{\n  \"timestamp_source\": \"%s\",\n  \"invariant_tsc\": %s,\n"
            "  \"pinned\": %s,\n  \"cycles_per_ns\": %.6f,\n  \"samples\": %" PRIu64 ",\n"
            "  \"min_round_trip_cycles\": %" PRIu64 ",\n"
            "  \"median_round_trip_cycles\": %" PRIu64 ",\n"
            "  \"offset_b_minus_a_cycles\": %" PRId64 ",\n"
            "  \"offset_a_minus_b_cycles\": %" PRId64 ",\n"
            "  \"uncertainty_cycles\": %" PRIu64 ",\n"
            "  \"largest_abs_offset_ns\": %.3f,\n"
            "  \"resolution_ns\": %.3f,\n"
            "  \"resolvable\": %s,\n"
            "  \"signs_oppose\": %s\n}\n",
            have_tsc ? "rdtsc" : "clock_gettime_monotonic",
            invariant ? "true" : "false", (pinned_a && pinned_b) ? "true" : "false",
            cyc_per_ns, samples, fwd.min_round_trip, fwd.median_round_trip,
            fwd.offset_cycles, rev.offset_cycles, fwd.bound_cycles, worst_ns,
            bound_ns, resolvable ? "true" : "false", signs_oppose ? "true" : "false");
        if (std::fclose(f) != 0) return 1;
    }

    // The gate is on what is KNOWN, which is the bound, not on the point
    // estimate. A run whose estimate is tiny because the method could not
    // resolve anything has not earned a pass.
    const double known_ns = resolvable ? worst_ns : bound_ns;
    if (max_offset_ns >= 0.0 && known_ns > max_offset_ns) {
        std::printf("\nFAIL: the offset is only bounded at %.1f ns, and this run was told to\n"
                    "accept %.1f ns. Either the clocks disagree by more than the caller can\n"
                    "tolerate, or the method cannot resolve finely enough to say otherwise --\n"
                    "and those are the same problem for anyone about to subtract them.\n",
                    known_ns, max_offset_ns);
        return 1;
    }
    return 0;
}
