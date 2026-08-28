#pragma once
//
// topology.hpp — the machine, read rather than assumed, three states at a time.
//
// ABSENT IS NEVER A PASS. This is the whole point of the file. A check written
// as "the governor must not be powersave" passes silently on a machine that has
// no cpufreq at all -- which is exactly the machine it exists to flag, since
// there is no cpufreq under WSL2. That is the same shape as the defect phase 10
// shipped: wire_to_book closed its socket before reading /proc/net/udp, so
// kernel_drops returned 0 on every run ever made and the exit code depending on
// it was unreachable. A gate whose input is a constant is not a gate.
//
// So every probe here returns one of three things -- a value, "this platform
// does not have it", or "it is here and could not be read" -- and the caller has
// to say what it does about each. cpu_jitter's read_status_long() already
// returns -1 for the second case and this follows it.
//
// WHY TOPOLOGY IS A VETO AND NOT A WARNING, for 12.8 specifically. Two
// single-threaded poll loops pinned to SMT SIBLINGS share one core's execution
// ports and one TSC. tsc_offset would then report an offset near zero and the
// run would look beautifully controlled while every hop was inflated by
// contention -- a check that cannot fail, again. Two CPUs on different physical
// PACKAGES is the mirror: cross-socket TSC synchronisation is a firmware
// property, and a dual-socket box can keep clocksource=tsc while the packages
// differ by far more than the offset bound, silently rewriting both
// cross-process hops. scripts/pinned-run.sh only warns about the first and says
// nothing about the second; here both are refusals.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace itchbook::bench {

enum class ProbeState { kValue, kAbsent, kUnreadable };

struct Probe {
    ProbeState state = ProbeState::kAbsent;
    std::string value;

    bool ok() const { return state == ProbeState::kValue; }
    // A probe that could not be read is NOT a probe that said no.
    bool is(const char* what) const { return ok() && value == what; }
    const char* state_name() const {
        switch (state) {
            case ProbeState::kValue: return "value";
            case ProbeState::kAbsent: return "absent";
            default: return "unreadable";
        }
    }
};

inline Probe read_probe(const std::string& path) {
    Probe p;
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) {
        // ENOENT is "this platform does not have it"; anything else is "it is
        // here and would not open". Both are refusals, but they are different
        // facts and an operator debugging a run needs to tell them apart.
        p.state = ProbeState::kAbsent;
        return p;
    }
    char buf[512];
    if (std::fgets(buf, sizeof(buf), f) == nullptr) {
        std::fclose(f);
        p.state = ProbeState::kUnreadable;
        return p;
    }
    std::fclose(f);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    p.state = ProbeState::kValue;
    p.value = s;
    return p;
}

inline Probe cpu_attr(int cpu, const char* leaf) {
    char path[256];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/%s", cpu, leaf);
    return read_probe(path);
}

// Everything 12.8 needs to know about one CPU before it will pin to it.
struct CpuTopology {
    int cpu = -1;
    Probe core_id;
    Probe thread_siblings;    // e.g. "6-7" or "6,7"
    Probe physical_package;
};

inline CpuTopology cpu_topology(int cpu) {
    CpuTopology t;
    t.cpu = cpu;
    t.core_id = cpu_attr(cpu, "topology/core_id");
    t.thread_siblings = cpu_attr(cpu, "topology/thread_siblings_list");
    t.physical_package = cpu_attr(cpu, "topology/physical_package_id");
    return t;
}

// Does `list` (a sysfs cpu list like "6-7" or "2,5,9-11") contain `cpu`?
inline bool cpu_list_contains(const std::string& list, int cpu) {
    size_t i = 0;
    while (i < list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        const std::string part = list.substr(i, j - i);
        const size_t dash = part.find('-');
        if (dash == std::string::npos) {
            if (!part.empty() && std::atoi(part.c_str()) == cpu) return true;
        } else {
            const int lo = std::atoi(part.substr(0, dash).c_str());
            const int hi = std::atoi(part.substr(dash + 1).c_str());
            if (cpu >= lo && cpu <= hi) return true;
        }
        i = j + 1;
    }
    return false;
}

struct PairVerdict {
    bool usable = false;
    std::string reason;      // why not, when not
};

// The two CPUs 12.8 will pin its two processes to. Both refusals below are
// vetoes rather than warnings; see the banner for why each one is a check that
// would otherwise be unable to fail.
inline PairVerdict check_pinned_pair(int cpu_a, int cpu_b) {
    PairVerdict v;
    if (cpu_a == cpu_b) {
        v.reason = "both processes on one CPU: the TCP hop would measure a "
                   "context switch and be publishable as a gateway latency";
        return v;
    }
    const CpuTopology a = cpu_topology(cpu_a);
    const CpuTopology b = cpu_topology(cpu_b);

    if (!a.thread_siblings.ok() || !b.thread_siblings.ok()) {
        v.reason = std::string("thread_siblings_list is ") +
                   a.thread_siblings.state_name() + "/" +
                   b.thread_siblings.state_name() +
                   ": cannot show the two CPUs are not SMT siblings, and absent "
                   "is not a pass";
        return v;
    }
    if (cpu_list_contains(a.thread_siblings.value, cpu_b)) {
        v.reason = "the two CPUs are SMT siblings: they share execution ports "
                   "and one TSC, so the offset would read near zero while every "
                   "hop was inflated by contention";
        return v;
    }
    if (!a.physical_package.ok() || !b.physical_package.ok()) {
        v.reason = std::string("physical_package_id is ") +
                   a.physical_package.state_name() + "/" +
                   b.physical_package.state_name() +
                   ": cannot show the two CPUs share a package, and cross-socket "
                   "TSC synchronisation is a firmware property";
        return v;
    }
    if (a.physical_package.value != b.physical_package.value) {
        v.reason = "the two CPUs are on different physical packages: a box can "
                   "keep clocksource=tsc while the packages differ by far more "
                   "than the offset bound";
        return v;
    }
    v.usable = true;
    return v;
}

// The environment fields every 12.8 artifact carries, so a verdict is
// recomputable from the object that reports it.
struct Environment {
    Probe clocksource;
    Probe governor;
    Probe no_turbo;
    Probe isolated;
    Probe cmdline;
    Probe kernel;
    Probe rmem_max;
};

inline Environment read_environment() {
    Environment e;
    e.clocksource = read_probe("/sys/devices/system/clocksource/clocksource0/current_clocksource");
    e.governor = cpu_attr(0, "cpufreq/scaling_governor");
    e.no_turbo = read_probe("/sys/devices/system/cpu/intel_pstate/no_turbo");
    e.isolated = read_probe("/sys/devices/system/cpu/isolated");
    e.cmdline = read_probe("/proc/cmdline");
    e.kernel = read_probe("/proc/version");
    e.rmem_max = read_probe("/proc/sys/net/core/rmem_max");
    return e;
}

// A hint, never a verdict on its own -- the decisive gate is cpu_jitter. Kept
// because a run that does not say it was under a hypervisor is a run whose
// other statements are worth less.
inline bool looks_virtualised(const Environment& e) {
    if (!e.kernel.ok()) return false;
    return e.kernel.value.find("microsoft-standard-WSL") != std::string::npos ||
           e.kernel.value.find("-hyperv") != std::string::npos;
}

inline void write_probe_json(std::FILE* f, const char* name, const Probe& p,
                             bool last = false) {
    std::fprintf(f, "    \"%s\": {\"state\": \"%s\", \"value\": ", name, p.state_name());
    if (p.ok()) {
        std::fprintf(f, "\"");
        for (char c : p.value) {
            if (c == '"' || c == '\\') std::fprintf(f, "\\%c", c);
            else if (static_cast<unsigned char>(c) < 0x20) std::fprintf(f, " ");
            else std::fputc(c, f);
        }
        std::fprintf(f, "\"");
    } else {
        std::fprintf(f, "null");
    }
    std::fprintf(f, "}%s\n", last ? "" : ",");
}

}  // namespace itchbook::bench
