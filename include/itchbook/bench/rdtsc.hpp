#pragma once
//
// rdtsc.hpp — cycle-accurate timing for the measured region.
//
// The rules the build plan sets for phase 4, and why each one matters:
//
//   * Store samples into a preallocated array. Never allocate, never print,
//     never touch a std::ostream inside the region being measured — a single
//     malloc is worth more cycles than the thing you are trying to observe.
//
//   * Serialise around the counter. rdtsc is not a barrier: out-of-order
//     execution will happily hoist work across it and hand you a nonsense
//     number. lfence before, rdtscp (which waits for prior instructions to
//     retire) plus lfence after.
//
// This is only meaningful on a CPU with an invariant TSC — constant_tsc and
// nonstop_tsc in /proc/cpuinfo. Without those the counter changes rate with
// frequency scaling and every measurement is fiction. `tsc_is_invariant()`
// checks, so a run on the wrong machine says so instead of lying.
//
#include <cstdint>
#include <cstdio>
#include <string>
#include <time.h>

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#define ITCHBOOK_HAVE_RDTSC 1
#else
#define ITCHBOOK_HAVE_RDTSC 0
#endif

namespace itchbook::bench {

#if ITCHBOOK_HAVE_RDTSC

// The CPU the last rdtscp ran on, alongside the counter. Linux puts
// (numa_node << 12) | cpu_id in IA32_TSC_AUX, so the low 12 bits are the CPU.
// Two paired stamps that disagree crossed cores between them.
inline uint64_t cycles_end_cpu(unsigned* cpu) {
    unsigned aux = 0;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    if (cpu != nullptr) *cpu = aux & 0xFFFu;
    return t;
}

// lfence, then rdtsc: nothing after this line starts before the read.
inline uint64_t cycles_begin() {
    _mm_lfence();
    return __rdtsc();
}

// rdtscp waits for everything before it to retire; the trailing lfence stops
// later work being pulled back across the read.
inline uint64_t cycles_end() {
    unsigned aux;
    uint64_t t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

#else  // no rdtsc: fall back to a clock, coarser but not wrong

// CLOCK_MONOTONIC was the original choice and it is the wrong one on Apple
// silicon, where its granularity is far coarser than the interval phase 10
// needs to see. tools/tsc_offset measured a cross-thread handoff there as
// ZERO nanoseconds -- not a fast handoff, a clock that cannot tell the two ends
// apart -- and every figure derived from it was arithmetic on rounding.
//
// CLOCK_UPTIME_RAW is Apple's un-adjusted counter and is both finer and
// cheaper: it does not go through the NTP-adjusted path CLOCK_MONOTONIC does.
// Everywhere else CLOCK_MONOTONIC is already fine, so the choice is made here
// rather than at the call sites.
//
// It is still a system-wide clock, which is the one advantage this fallback has
// over the TSC: there is no per-core offset to worry about. What it does not
// give back is resolution, so anything measuring it should ask
// tsc_offset --samples N what one tick costs before trusting a difference.
#if defined(__APPLE__)
#define ITCHBOOK_FALLBACK_CLOCK CLOCK_UPTIME_RAW
#else
#define ITCHBOOK_FALLBACK_CLOCK CLOCK_MONOTONIC
#endif

inline uint64_t cycles_begin() {
    timespec ts;
    clock_gettime(ITCHBOOK_FALLBACK_CLOCK, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}
inline uint64_t cycles_end() { return cycles_begin(); }

// No rdtscp, so no CPU to report. Absent, not zero -- a caller that reads this
// as "cpu 0" would conclude two stamps agreed when neither was measured.
inline uint64_t cycles_end_cpu(unsigned* cpu) {
    if (cpu != nullptr) *cpu = 0xFFFFu;   // sentinel: unavailable
    return cycles_end();
}

#endif

// What a build is ACTUALLY timing with, for anything that reports it.
//
// A tool that prints the wrong clock's name is a tool whose other statements
// are worth less: tsc_offset said "CLOCK_MONOTONIC" for a build that had
// already been switched to CLOCK_UPTIME_RAW, which is a small lie about a
// measurement in a project whose entire argument is that it does not tell them.
inline const char* clock_name() {
#if ITCHBOOK_HAVE_RDTSC
    return "rdtsc / rdtscp (per-core counter)";
#elif defined(__APPLE__)
    return "clock_gettime CLOCK_UPTIME_RAW (system-wide)";
#else
    return "clock_gettime CLOCK_MONOTONIC (system-wide)";
#endif
}

// True when the TSC ticks at a fixed rate regardless of frequency and C-state.
// Without it, cycle counts are not comparable across a run.
inline bool tsc_is_invariant() {
#if ITCHBOOK_HAVE_RDTSC
    std::FILE* f = std::fopen("/proc/cpuinfo", "r");
    if (f == nullptr) return false;   // not Linux; caller decides what to do
    char line[4096];
    bool constant = false;
    bool nonstop = false;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string s(line);
        if (s.rfind("flags", 0) != 0) continue;
        constant = s.find("constant_tsc") != std::string::npos;
        nonstop = s.find("nonstop_tsc") != std::string::npos;
        break;
    }
    std::fclose(f);
    return constant && nonstop;
#else
    return false;
#endif
}

// ONE EPOCH AND ONE SCALE, SHARED BY EVERY PROCESS ON THE MACHINE.
//
// Both 12.7 tools hand-rolled this identically. 12.8 takes twelve stamps across
// two processes and subtracts them, so which clock is not a detail: rdtsc is
// per-core and each process would need its own cycles-per-ns calibration, and
// two 50 ms calibrations of the same counter differ by ~1e-4 relative -- a
// second cross-process error term underneath the one everyone is looking at.
//
// The cost objection does not survive measurement. Net of the instrument's own
// 28-cycle zero, on this box at 3.599 GHz: clock_gettime(CLOCK_MONOTONIC) 40
// cycles, serialised rdtscp 40 cycles. A wash.
//
// What this does NOT buy, because claiming it would repeat the error
// tools/tsc_offset.cpp exists to correct: on x86 with clocksource=tsc the vDSO
// computes CLOCK_MONOTONIC as a GLOBAL mult/shift/offset over a raw rdtsc, with
// no per-CPU correction term. Cross-core skew passes straight through. What is
// bought is one epoch and one scale for free, plus the kernel's own TSC
// synchronisation check and watchdog.
inline uint64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// CPU time consumed by THIS THREAD, for telling "the code was slow" apart from
// "the thread was not running".
//
// wall_elapsed - thread_cpu_elapsed over a window is the time the thread spent
// off-CPU in it, which is what phase 12.8 needs to know before it prints a tail
// percentile: on a box whose median scheduler gap is 15 us, a p99.9 over ~1,200
// samples is mostly deschedules wearing a hop's label.
//
// Measured on this machine rather than assumed: observed granularity 131 ns
// (clock_getres claims 1), and it costs ~522 cycles against ~68 for a
// CLOCK_MONOTONIC read -- so it is read at chain boundaries and never per stamp.
inline uint64_t thread_cpu_ns() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Cycles per nanosecond, measured rather than assumed: the nominal clock in the
// model name is not necessarily the TSC rate.
inline double calibrate_cycles_per_ns(unsigned millis = 50) {
    timespec t0;
    timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t c0 = cycles_begin();

    timespec sleep_for{0, static_cast<long>(millis) * 1000000L};
    nanosleep(&sleep_for, nullptr);

    uint64_t c1 = cycles_end();
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double ns = static_cast<double>(t1.tv_sec - t0.tv_sec) * 1e9 +
                static_cast<double>(t1.tv_nsec - t0.tv_nsec);
    if (ns <= 0.0) return 1.0;
    return static_cast<double>(c1 - c0) / ns;
}

}  // namespace itchbook::bench
