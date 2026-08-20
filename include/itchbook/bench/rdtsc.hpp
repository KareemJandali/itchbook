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

#else  // no rdtsc: fall back to the clock, coarser but not wrong

inline uint64_t cycles_begin() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}
inline uint64_t cycles_end() { return cycles_begin(); }

#endif

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
