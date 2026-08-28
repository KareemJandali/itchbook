//
// test_tick_to_trade.cpp — the phase-12.8 measurement machinery, tested for the
// properties the numbers depend on.
//
// These are not tests that the timings are right; no unit test can say that.
// They are tests of the things that would make a timing SILENTLY WRONG: an
// arena that wraps instead of dropping, a saturated histogram that looks like a
// large sample, a topology probe that treats "absent" as "fine", and a trace
// file whose layout has drifted from the reader that parses it.
//
#include "tests/check.hpp"

#include <cstdio>
#include <cstring>
#include <time.h>
#include <string>
#include <vector>

#include "itchbook/bench/histogram.hpp"
#include "itchbook/bench/rdtsc.hpp"
#include "itchbook/bench/topology.hpp"
#include "itchbook/bench/trace.hpp"

using namespace itchbook::bench;

// ---- the arena drops, and never wraps ----------------------------------------
//
// A wrapped index pairs one order's t0 with another order's t3, and the result
// looks exactly like a plausible latency -- there is nothing about the number a
// reader could notice. So overflow must be visible.
static void test_arena_drops_rather_than_wraps() {
    Arena<ChainA> a;
    a.reserve(4);
    // DISTINCT slots. Checking only for non-null let a mutation that returned
    // &store_[0] every time -- the exact aliasing this test is named for -- pass
    // every assertion.
    ChainA* seen[4] = {};
    for (int i = 0; i < 4; ++i) {
        seen[i] = a.push();
        CHECK(seen[i] != nullptr);
        seen[i]->t0 = uint64_t(1000 + i);
    }
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
            CHECK(seen[i] != seen[j]);
    // ...and each slot still holds its own value, which aliasing would destroy.
    for (int i = 0; i < 4; ++i) CHECK_EQ(seen[i]->t0, uint64_t(1000 + i));
    CHECK_EQ(a.size(), size_t(4));
    CHECK(!a.reserve_exceeded());

    // Past the end: nothing is written, and the loss is counted.
    CHECK(a.push() == nullptr);
    CHECK(a.push() == nullptr);
    CHECK_EQ(a.dropped(), uint64_t(2));
    CHECK(a.reserve_exceeded());
    CHECK_EQ(a.size(), size_t(4));           // it did NOT grow
    CHECK_EQ(a.high_water(), size_t(4));

    // Direct indexing refuses out of range too, rather than aliasing slot 0.
    Arena<ChainA> b;
    b.reserve(8);
    ChainA* first = b.at(0);
    ChainA* third = b.at(2);
    CHECK(first != nullptr);
    CHECK(third != nullptr);
    CHECK(first != third);              // at() must index, not alias
    first->t0 = 12345;
    if (third != nullptr) third->t0 = 777;
    CHECK_EQ(b.at(2)->t0, uint64_t(777));
    CHECK(b.at(8) == nullptr);
    CHECK(b.at(9999) == nullptr);
    CHECK_EQ(b.dropped(), uint64_t(2));
    CHECK_EQ(b.at(0)->t0, uint64_t(12345));  // untouched by the refusals
}

// ---- a clamped histogram says so ---------------------------------------------
//
// A max of exactly UINT32_MAX is indistinguishable from a real one. The resting
// interval reaches the clamp at low replay multipliers, so this is not
// hypothetical.
static void test_histogram_reports_saturation() {
    Histogram h(16);
    h.add(10);
    h.add(20);
    CHECK_EQ(h.saturated(), uint64_t(0));

    // Exactly at the boundary: UINT32_MAX is representable, so it is NOT a
    // clamped sample and must not be counted as one. Only testing 2^32 and 2^40
    // left an off-by-one in the predicate invisible.
    h.add(uint64_t(UINT32_MAX));
    CHECK_EQ(h.saturated(), uint64_t(0));
    h.add(uint64_t(UINT32_MAX) + 1);
    CHECK_EQ(h.saturated(), uint64_t(1));
    h.add(uint64_t(1) << 40);
    CHECK_EQ(h.saturated(), uint64_t(2));
    CHECK_EQ(h.count(), size_t(5));
    h.finalize();
    CHECK_EQ(h.max(), UINT32_MAX);   // the value is clamped...
    CHECK_EQ(h.saturated(), uint64_t(2));   // ...and the clamping is reported
}

// ---- absent is never a pass ---------------------------------------------------
//
// There is no cpufreq under WSL2 at all, so a check written as "the governor
// must not be powersave" passes silently on the machine it exists to flag. The
// three-state probe is what stops that.
static void test_probe_absent_is_not_a_value() {
    Probe p = read_probe("/proc/does/not/exist/at/all");
    CHECK(!p.ok());
    CHECK(!p.is("anything"));            // absent must not satisfy a value test
    CHECK(!p.is(""));                    // not even the empty string
    CHECK(std::string(p.state_name()) == "absent");

    // A file we WRITE, so the value case is asserted unconditionally. Every
    // assertion here used to be negative, so a read_probe stubbed to return
    // ABSENT for everything -- the whole topology layer dead -- passed the lot.
    const char* tmp = "/tmp/itchbook_probe_test.txt";
    std::FILE* w = std::fopen(tmp, "w");
    CHECK(w != nullptr);
    if (w != nullptr) {
        std::fputs("performance\n", w);
        std::fclose(w);
        Probe v = read_probe(tmp);
        CHECK(v.ok());                                   // it MUST read a value
        CHECK(std::string(v.state_name()) == "value");
        CHECK(v.value == "performance");                 // and the right one
        CHECK(v.is("performance"));                      // is() must say yes
        CHECK(!v.is("powersave"));                       // and no to the wrong one
        std::remove(tmp);
    }
}

// ---- the cpu-list parser the SMT veto rests on --------------------------------
static void test_cpu_list_membership() {
    CHECK(cpu_list_contains("6-7", 6));
    CHECK(cpu_list_contains("6-7", 7));
    CHECK(!cpu_list_contains("6-7", 5));
    CHECK(!cpu_list_contains("6-7", 8));
    CHECK(cpu_list_contains("2,5,9-11", 2));
    CHECK(cpu_list_contains("2,5,9-11", 5));
    CHECK(cpu_list_contains("2,5,9-11", 10));
    CHECK(!cpu_list_contains("2,5,9-11", 6));
    CHECK(!cpu_list_contains("", 0));       // empty list contains nothing
    CHECK(cpu_list_contains("13", 13));
    CHECK(!cpu_list_contains("13", 1));     // not a prefix match
}

// ---- the pinning vetoes -------------------------------------------------------
//
// Two poll loops on SMT siblings share execution ports AND one TSC, so
// tsc_offset would read near zero while every hop was inflated by contention --
// a check that cannot fail. Same CPU twice is worse: the cross-process hop
// becomes a context switch, publishable as a gateway latency.
static void test_pinned_pair_refusals() {
    PairVerdict same = check_pinned_pair(3, 3);
    CHECK(!same.usable);
    CHECK(same.reason.find("one CPU") != std::string::npos);

    // A CPU that cannot exist: its topology is unreadable, and unreadable must
    // never clear. This is the assertion that does not depend on the machine --
    // the previous version only checked the sibling veto inside an `if` on the
    // runner's own sysfs, so a CI box reporting no SMT skipped it silently.
    PairVerdict nonexistent = check_pinned_pair(0, 9999);
    CHECK(!nonexistent.usable);
    CHECK(nonexistent.reason.find("absent") != std::string::npos ||
          nonexistent.reason.find("siblings") != std::string::npos ||
          nonexistent.reason.find("package") != std::string::npos);

    // Where the machine DOES publish topology, the sibling veto must fire on a
    // real sibling pair. Reported, not skipped, when it cannot be exercised.
    Probe sib = cpu_attr(0, "topology/thread_siblings_list");
    if (sib.ok() && cpu_list_contains(sib.value, 1)) {
        PairVerdict adj = check_pinned_pair(0, 1);
        CHECK(!adj.usable);
        CHECK(adj.reason.find("sibling") != std::string::npos);
    } else {
        std::printf("  (note: cpu0/cpu1 are not siblings here, so the sibling "
                    "veto was exercised only through cpu_list_contains)\n");
    }
}

// ---- the trace file the report parses -----------------------------------------
//
// The reader is a Python script matching these struct layouts by hand. A silent
// change here would be read there as plausible numbers rather than as an error,
// so the sizes are pinned and a round trip is exercised.
static void test_trace_layout_and_round_trip() {
    // Pinned to what scripts/phase12-8-report.py unpacks. These catch a
    // padding change inside C++; tests/test_report_join.py catches the two
    // languages disagreeing, by reading the fixture this test writes.
    CHECK_EQ(sizeof(ChainA), size_t(112));    // "<QQQQQQQQQQQIIIIHHBBBB"
    CHECK_EQ(sizeof(FillRec), size_t(32));    // "<QQIIIBBH"
    CHECK_EQ(sizeof(AcceptEx), size_t(24));   // "<QQIB3x"
    CHECK_EQ(sizeof(EmitEx), size_t(32));     // "<QQQII"
    CHECK_EQ(sizeof(PktEx), size_t(24));      // "<QQII"

    Arena<AcceptEx> ac;
    ac.reserve(4);
    for (uint32_t i = 1; i <= 3; ++i) {
        AcceptEx* e = ac.push();
        CHECK(e != nullptr);
        if (e == nullptr) return;
        e->t3p = 1000 + i;
        e->t4 = 2000 + i;
        e->token_seq = i;
        e->resp = 'A';
    }

    const char* path = "/tmp/itchbook_test_trace.bin";
    std::FILE* f = trace_open(path);
    CHECK(f != nullptr);
    if (f == nullptr) return;
    CHECK(write_section(f, "ACPT", ac, ac.size()));
    std::fclose(f);

    // Read it back the way the report does: magic, version, then tagged
    // sections of fixed-width records.
    std::FILE* r = std::fopen(path, "rb");
    CHECK(r != nullptr);
    if (r == nullptr) return;
    uint32_t magic = 0, ver = 0;
    CHECK_EQ(std::fread(&magic, sizeof(magic), 1, r), size_t(1));
    CHECK_EQ(std::fread(&ver, sizeof(ver), 1, r), size_t(1));
    CHECK_EQ(magic, kTraceMagic);
    CHECK_EQ(ver, kTraceVersion);

    char tag[5] = {};
    uint32_t n = 0, rec = 0;
    CHECK_EQ(std::fread(tag, 1, 4, r), size_t(4));
    CHECK_EQ(std::fread(&n, sizeof(n), 1, r), size_t(1));
    CHECK_EQ(std::fread(&rec, sizeof(rec), 1, r), size_t(1));
    CHECK(std::string(tag) == "ACPT");
    CHECK_EQ(n, uint32_t(3));
    CHECK_EQ(rec, uint32_t(sizeof(AcceptEx)));

    AcceptEx back[3];
    CHECK_EQ(std::fread(back, sizeof(AcceptEx), 3, r), size_t(3));
    std::fclose(r);
    std::remove(path);
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK_EQ(back[i].token_seq, i + 1);
        CHECK_EQ(back[i].t3p, uint64_t(1001 + i));
        CHECK_EQ(back[i].t4, uint64_t(2001 + i));
        CHECK_EQ(back[i].resp, uint8_t('A'));
    }
}

// ---- the clocks the stamps are taken with -------------------------------------
static void test_clocks_move_the_right_way() {
    const uint64_t a = mono_ns();
    const uint64_t b = mono_ns();
    CHECK(b >= a);                       // monotonic, by name and by contract

    // Thread CPU time advances too, and never faster than the wall over the
    // same window -- the census's whole premise is wall >= cpu.
    // (a) While the thread WORKS, the CPU clock must advance -- a stub returning
    // a constant zero used to pass this whole test.
    const uint64_t w0 = mono_ns(), c0 = thread_cpu_ns();
    uint64_t spin = 0;
    while (mono_ns() - w0 < 2'000'000ULL) ++spin;   // 2 ms of real work
    const uint64_t w1 = mono_ns(), c1 = thread_cpu_ns();
    CHECK(w1 > w0);
    CHECK(spin > 0);
    const uint64_t wall = w1 - w0, cpu = c1 - c0;
    CHECK(cpu > 0);                       // it MOVES
    CHECK(cpu > wall / 4);                // and it tracks work, not nothing
    CHECK(cpu <= wall + 1'000'000ULL);    // without running away

    // (b) While the thread SLEEPS, the CPU clock must NOT advance. This is the
    // property the gap-overlap census rests on, and the one a wall clock cannot
    // fake: replacing CLOCK_THREAD_CPUTIME_ID with CLOCK_MONOTONIC used to pass.
    const uint64_t sw0 = mono_ns(), sc0 = thread_cpu_ns();
    timespec nap{0, 20'000'000L};         // 20 ms asleep
    nanosleep(&nap, nullptr);
    const uint64_t sw1 = mono_ns(), sc1 = thread_cpu_ns();
    const uint64_t slept_wall = sw1 - sw0, slept_cpu = sc1 - sc0;
    CHECK(slept_wall > 15'000'000ULL);    // we really did sleep
    // A wall clock would report ~20 ms of "CPU" here. A thread CPU clock reports
    // almost none, because the thread was not running.
    CHECK(slept_cpu < slept_wall / 4);
    if (slept_cpu >= slept_wall / 4) {
        std::fprintf(stderr, "  thread CPU advanced %llu ns across a %llu ns "
                             "sleep: this is not a per-thread CPU clock\n",
                     (unsigned long long)slept_cpu, (unsigned long long)slept_wall);
    }

    // (c) The migration witness. On a build with rdtscp it must report a
    // PLAUSIBLE cpu id; elsewhere it must report the unavailable sentinel. The
    // old check accepted either, so a platform reporting "unavailable" for every
    // stamp -- the migration witness permanently blind -- passed.
    unsigned cpu_id = 12345;
    cycles_end_cpu(&cpu_id);
#if ITCHBOOK_HAVE_RDTSC
    CHECK(cpu_id < 4096u);                // a real cpu id, not the sentinel
#else
    CHECK(cpu_id == 0xFFFFu);             // explicitly unavailable, not zero
#endif
}

// A trace with one record of every section, at values a reader can recognise.
// tests/test_report_join.py parses this with the report's own format strings, so
// the two languages are checked against each other rather than against two
// copies of a constant.
static void write_fixture() {
    Arena<ChainA> ca;   ca.reserve(1);
    Arena<FillRec> fr;  fr.reserve(1);
    Arena<AcceptEx> ac; ac.reserve(1);
    Arena<EmitEx> em;   em.reserve(1);
    Arena<PktEx> pk;    pk.reserve(1);

    ChainA* c = ca.push();
    c->t0 = 1; c->t1 = 2; c->t1p = 3; c->t2 = 4; c->t3 = 5;
    c->iter_start = 6; c->iter_end = 7; c->tsc0 = 8; c->tsc3 = 9;
    c->cpu_t1p = 10; c->cpu_t3 = 11;
    c->ref_seq = 12; c->stride = 13; c->dgrams_after_trigger = 14;
    c->msgs_after_trigger = 15; c->cpu0 = 16; c->cpu3 = 17;
    c->resp = 'A'; c->have = 31; c->terminal = 2;

    // Every field a DIFFERENT number, so a swap of any two shows up in
    // tests/test_report_join.py rather than passing unnoticed. Swapping
    // ref_seq and fill_ordinal inverts chain B's join key and used to pass.
    FillRec* f = fr.push();
    f->t6p = 21; f->t6 = 22; f->ref_seq = 23; f->fill_ordinal = 24;
    f->shares = 25; f->in_book = 1; f->parked = 1; f->pad = 0;

    AcceptEx* a = ac.push();
    a->t3p = 31; a->t4 = 32; a->token_seq = 33; a->resp = 'J';

    EmitEx* e = em.push();
    e->tA = 41; e->t5a = 42; e->mold_seq = 43; e->ref_seq = 44; e->fill_ordinal = 45;

    PktEx* p = pk.push();
    p->header_seq = 51; p->t5b = 52; p->header_count = 53;

    std::FILE* f2 = trace_open("/tmp/itchbook_trace_fixture.bin");
    if (f2 == nullptr) { CHECK(false); return; }
    CHECK(write_section(f2, "CHNA", ca, 1));
    CHECK(write_section(f2, "FILL", fr, 1));
    CHECK(write_section(f2, "ACPT", ac, 1));
    CHECK(write_section(f2, "EMIT", em, 1));
    CHECK(write_section(f2, "PKTS", pk, 1));
    std::fclose(f2);
}

int main() {
    test_arena_drops_rather_than_wraps();
    test_histogram_reports_saturation();
    test_probe_absent_is_not_a_value();
    test_cpu_list_membership();
    test_pinned_pair_refusals();
    test_trace_layout_and_round_trip();
    test_clocks_move_the_right_way();
    write_fixture();

    if (itchbook::test::failures == 0) {
        std::printf("test_tick_to_trade: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "test_tick_to_trade: %d failure(s)\n",
                 itchbook::test::failures);
    return 1;
}
