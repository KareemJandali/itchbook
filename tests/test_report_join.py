#!/usr/bin/env python3
"""tests/test_report_join.py — the report's join, sign gates and census, on
inputs whose answers are known.

A live run cannot test these. It produces whatever it produces, and if the join
silently paired the wrong records or the census silently never fired, the output
would still look like a table of plausible latencies. So this builds traces by
hand with defects planted in them and requires the report to refuse each one.

THE FIRST VERSION OF THIS FILE WAS LARGELY DECORATION, and an adversarial audit
proved it by mutation. Every design rule below exists because a specific
mutation went unnoticed:

  * ASSERT ON --json-out, NEVER ON PROSE. `check("duplicate" in out)` passed
    because the report prints a "duplicate tokens" HEADER on every run, clean or
    not; `check("40" in out)` passed because the report echoes `orders sent 40`.
    Substring checks against a page of prose are satisfied by the page. The
    report emits a machine-readable verdict; that is what gets asserted.

  * THE ARRAY INDEX MUST DIFFER FROM ref_seq. Every fixture used to set them
    equal, so the report's join key could not be observed at all: changing
    `chains[i]` to `chains[ref_seq]` left every test passing. They are now
    deliberately different numbers.

  * A FIXTURE MUST VARY ONLY THE THING UNDER TEST. The census fixture used to
    make the descheduled chains differ in BOTH wall time and CPU time, so a
    census that ignored the CPU clock entirely and thresholded wall time gave
    the identical answer -- and deleting the CPU term from the report went
    unnoticed. The stopped chains now have the SAME wall time as the clean ones
    and differ only in CPU consumed.

  * AN EXIT CODE WITH MANY CAUSES PROVES NOTHING. `rc == 3` passed whether or
    not the census gate existed, because no --jitter-json always adds its own
    reason to be unquotable. Every run here supplies a clean jitter artifact, so
    the reason under test is the only one that can fire.
"""
import json
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REPORT = os.path.join(ROOT, "scripts", "phase12-8-report.py")

MAGIC = 0x38324254
VERSION = 1
CHAINA = "<QQQQQQQQQQQIIIIHHBBBB"
FILLREC = "<QQIIIBBH"
ACCEPTEX = "<QQIB3x"
EMITEX = "<QQQII"
PKTEX = "<QQII"

HAVE_ALL = 1 | 2 | 4 | 8 | 16
# The exchange reference is a DIFFERENT dense counter from the order token, and
# the join must use the token. Offsetting one from the other is what makes a
# join on the wrong key observable.
REF_OFFSET = 500
# The paired rdtscp stamps have to be consistent with the nanosecond ones or the
# report's two-instrument check reports a disagreement -- and a fixture meant to
# be the happy path must pass every gate, or the baseline tests something else.
CYC_PER_NS = 3.6

failures = []


def check(cond, what):
    if cond:
        print("  ok    %s" % what)
    else:
        print("  FAIL  %s" % what)
        failures.append(what)


def section(tag, fmt, rows):
    body = b"".join(struct.pack(fmt, *r) for r in rows)
    return tag.encode() + struct.pack("<II", len(rows), struct.calcsize(fmt)) + body


def write_trace(path, sections):
    with open(path, "wb") as f:
        f.write(struct.pack("<II", MAGIC, VERSION))
        for tag, fmt, rows in sections:
            f.write(section(tag, fmt, rows))


def chain(t0=1000, t1=2000, t1p=3000, t2=4000, t3=5000,
          iter_start=900, iter_end=5100, tsc0=0, tsc3=0,
          cpu_t1p=0, cpu_t3=0, ref=0, stride=200, dg=1, msgs=10,
          cpu0=0xFFFF, cpu3=0xFFFF, resp=ord('A'), have=HAVE_ALL, term=1):
    return (t0, t1, t1p, t2, t3, iter_start, iter_end, tsc0, tsc3,
            cpu_t1p, cpu_t3, ref, stride, dg, msgs, cpu0, cpu3, resp, have, term, 0)


def empty_chain():
    """Slot 0: the strategy mints tokens from 1, so index 0 carries have=0."""
    return chain(have=0)


def chains_for(tokens, **kw):
    """The array as the strategy lays it out: unused slot 0, then one chain per
    token AT THE INDEX ITS TOKEN NAMES. ref_seq is deliberately a different
    number, so a join on the wrong key shows up."""
    rows = [empty_chain()]
    for i in tokens:
        vals = {k: (v(i) if callable(v) else v) for k, v in kw.items()}
        vals.setdefault("ref", i + REF_OFFSET)
        rows.append(chain(**vals))
    return rows


CLEAN_JITTER = {"cpus": [{"cpu": 13, "gaps_over": {"100us": 0}, "gap_ns": {"max": 900}},
                         {"cpu": 14, "gaps_over": {"100us": 0}, "gap_ns": {"max": 850}}]}


def run_report(st_rows, ex_rows, fills=None, emits=None, pkts=None,
               st_json=None, ex_json=None, extra=(), jitter=CLEAN_JITTER):
    """Returns (exit code, parsed --json-out, stdout). Assertions use the JSON."""
    d = tempfile.mkdtemp()
    stp, exp = os.path.join(d, "st.trace"), os.path.join(d, "ex.trace")
    st_sections = [("CHNA", CHAINA, st_rows)]
    if fills is not None:
        st_sections.append(("FILL", FILLREC, fills))
    ex_sections = [("ACPT", ACCEPTEX, ex_rows)]
    if emits is not None:
        ex_sections.append(("EMIT", EMITEX, emits))
    if pkts is not None:
        ex_sections.append(("PKTS", PKTEX, pkts))
    write_trace(stp, st_sections)
    write_trace(exp, ex_sections)

    real = sum(1 for r in st_rows if r[18] != 0)
    sj, ej = os.path.join(d, "st.json"), os.path.join(d, "ex.json")
    with open(sj, "w") as f:
        json.dump(st_json or {"orders_sent": real, "samples_dropped": 0,
                              "core_migrations": 0, "gaps": 0, "messages_lost": 0}, f)
    with open(ej, "w") as f:
        json.dump(ex_json or {"samples_dropped": 0}, f)

    # A CLEAN jitter artifact, so the only reason a run can be unquotable is the
    # one the test is about. Without this every run is unquotable for a reason
    # that has nothing to do with the check being made.
    args = [sys.executable, REPORT, "--strategy-trace", stp, "--exchange-trace", exp,
            "--strategy-json", sj, "--exchange-json", ej, "--offset-bound-ns", "52"]
    if jitter is not None:
        jp = os.path.join(d, "jitter.json")
        with open(jp, "w") as f:
            json.dump(jitter, f)
        args += ["--jitter-json", jp]
    out_json = os.path.join(d, "report.json")
    args += ["--json-out", out_json]
    args += list(extra)

    r = subprocess.run(args, capture_output=True, text=True)
    doc = None
    if os.path.exists(out_json):
        with open(out_json) as f:
            doc = json.load(f)
    return r.returncode, doc, r.stdout + r.stderr


def broken_mentions(doc, phrase):
    return doc is not None and any(phrase in b for b in doc.get("broken", []))


def unquotable_mentions(doc, phrase):
    return doc is not None and any(phrase in b for b in doc.get("not_quotable", []))


# ---- 1. the happy path, and the join actually observed -------------------------
def joinable_fixture(tokens):
    """Each chain gets its OWN t3, and its exchange record is t3 + 300.

    A correct join therefore makes EVERY t3->t3' exactly 300. Any mispairing
    makes it something else -- which the earlier version of this fixture could
    not detect, because with a constant chain-side t3 the multiset of
    differences is invariant under permuting the exchange side."""
    st = [empty_chain()]
    ex = []
    for i in tokens:
        t0, t3 = 1000, 5000 + 10 * i
        st.append(chain(ref=i + REF_OFFSET, t0=t0, t1=2000, t1p=3000, t2=4000, t3=t3,
                        iter_start=900, iter_end=t3 + 100,
                        tsc0=1_000_000,
                        tsc3=1_000_000 + int(CYC_PER_NS * (t3 - t0)),
                        cpu_t1p=100_000 + i * 100_000,
                        cpu_t3=100_000 + i * 100_000 + (t3 - 3000)))
        ex.append((t3 + 2000, t3 + 2300, i, ord("A")))
    return st, ex


def test_clean_run_joins_the_right_records():
    st, ex = joinable_fixture(range(1, 41))
    rc, doc, out = run_report(st, ex)
    check(rc == 0, "a clean run with a clean jitter artifact is QUOTABLE (exit %d)" % rc)
    check(doc is not None and doc.get("quotable") is True, "and says so in its JSON")
    check(doc is not None and doc.get("joined") == 40, "it joined all forty orders")
    check(doc is not None and not doc.get("broken"), "nothing is reported broken")
    hop = (doc or {}).get("hops", {})
    tcp = next((v for k, v in hop.items() if "t3->t3" in k), None)
    check(tcp is not None and tcp["p50"] == 2000,
          "every chain paired with ITS OWN exchange record, so the TCP hop is "
          "exactly 2000 (got %s)" % (tcp or {}).get("p50"))


def test_a_mispaired_join_is_visible():
    """The control. Pair each chain with a DIFFERENT order's exchange record and
    require the reported hop to stop being the constant. Without this, the test
    above could pass on any join at all."""
    st, ex = joinable_fixture(range(1, 41))
    # Rotate the token each exchange record claims: record for token i now
    # carries the stamps that belong to token i+1.
    rotated = []
    for idx, (t3p, t4, tok, resp) in enumerate(ex):
        rotated.append((t3p, t4, (tok % 40) + 1, resp))
    rc, doc, out = run_report(st, rotated)
    hop = (doc or {}).get("hops", {})
    tcp = next((v for k, v in hop.items() if "t3->t3" in k), None)
    check(tcp is not None and tcp["p50"] != 2000,
          "rotating the pairing stops the TCP hop being the constant 2000 "
          "(got %s), so the join key is observable" % (tcp or {}).get("p50"))


# ---- 2. a negative intra-process hop is a harness bug, and must abort ----------
def test_negative_intra_process_hop_is_fatal():
    st = chains_for(range(1, 41))
    st[8] = chain(ref=8 + REF_OFFSET, t2=4000, t3=3500)   # t3 before t2
    ex = [(5000 + 10 * i, 5000 + 10 * i + 300, i, ord('A')) for i in range(1, 41)]
    rc, doc, out = run_report(st, ex)
    check(rc == 1, "a negative intra-process hop is BROKEN, not merely unquotable")
    check(broken_mentions(doc, "NEGATIVE intra-process"),
          "and the JSON names it as such")


# ---- 3. the join key must identify exactly one order --------------------------
def test_exchange_record_with_no_chain_is_fatal():
    st = chains_for(range(1, 41))
    ex = [(5000, 5300, i, ord('A')) for i in range(1, 41)]
    ex.append((5000, 5300, 9999, ord('A')))
    rc, doc, out = run_report(st, ex)
    check(rc == 1, "an exchange record joining to no chain is BROKEN")
    check(broken_mentions(doc, "join to no chain"), "and the JSON says so")


def test_duplicate_token_is_fatal():
    st = chains_for(range(1, 41))
    ex = [(5000, 5300, i, ord('A')) for i in range(1, 41)]
    ex.append((5100, 5400, 7, ord('A')))
    rc, doc, out = run_report(st, ex)
    check(rc == 1, "a duplicate order token is BROKEN")
    check(broken_mentions(doc, "appeared twice"),
          "and the JSON names the duplicated token, not a header line")


# ---- 4. chain B: the two independently-counted ordinals must agree ------------
def test_fill_the_exchange_never_saw_is_fatal():
    st = chains_for(range(1, 41))
    ex = [(5000, 5300, i, ord('A')) for i in range(1, 41)]
    pkts = [(1, 6000, 10, 0)]
    emits = [(5200, 5400, 1 + i, 1 + REF_OFFSET + i, 0) for i in range(5)]
    fills = [(6100, 6200, 1 + REF_OFFSET + i, 0, 100, 1, 0, 0) for i in range(5)]
    fills.append((6100, 6200, 999, 7, 100, 1, 0, 0))   # no exchange record
    rc, doc, out = run_report(st, ex, fills=fills, emits=emits, pkts=pkts)
    check(rc == 1, "a fill with no matching exchange record is BROKEN")
    check(broken_mentions(doc, "no matching exchange"),
          "and the JSON names the ordinal disagreement")


# ---- 5. the census must fire, and must be using the CPU clock ----------------
def test_census_tags_only_the_chains_that_stopped():
    """The stopped chains have the SAME WALL TIME as the clean ones and differ
    only in CPU consumed. A census that thresholded wall time -- or ignored the
    CPU clock -- would tag nothing and fail this test."""
    st = [empty_chain()]
    # Thirty clean: 50,000 ns of wall, 49,000 ns of CPU.
    for i in range(1, 31):
        st.append(chain(ref=i + REF_OFFSET, t1p=3000, t3=53000,
                        cpu_t1p=100000 + i * 100000,
                        cpu_t3=100000 + i * 100000 + 49000))
    # Ten stopped: the SAME 50,000 ns of wall, but only 1,000 ns of CPU.
    for i in range(31, 41):
        st.append(chain(ref=i + REF_OFFSET, t1p=3000, t3=53000,
                        cpu_t1p=100000 + i * 100000,
                        cpu_t3=100000 + i * 100000 + 1000))
    ex = [(53500, 53800, i, ord('A')) for i in range(1, 41)]
    rc, doc, out = run_report(st, ex)
    cen = (doc or {}).get("gap_census", {})
    check(cen.get("available") is True, "the census ran")
    check(cen.get("tagged") == 10,
          "it tagged exactly the ten that stopped running (got %s), and the "
          "clean ones have identical wall time so wall alone cannot explain it"
          % cen.get("tagged"))
    check(rc != 1, "tagging is not itself an error (exit %d)" % rc)


def test_census_absent_means_no_p999():
    st = chains_for(range(1, 41), cpu_t1p=0, cpu_t3=0)
    ex = [(5000, 5300, i, ord('A')) for i in range(1, 41)]
    rc, doc, out = run_report(st, ex)
    cen = (doc or {}).get("gap_census", {})
    check(cen.get("available") is False, "a missing census is reported as absent")
    check(unquotable_mentions(doc, "no gap-overlap census"),
          "and THAT is the reason the run is unquotable, not some other reason")
    check(rc == 3, "the run is not quotable without it (exit %d)" % rc)
    check(doc is not None and doc.get("quotable") is False, "and the JSON agrees")


# ---- 6. every field of every record, both languages ---------------------------
def test_fixture_round_trips_from_cpp():
    """The C++ test writes one record of each section with a DISTINCT value in
    every field. Checking a few fields let three struct-field swaps through, so
    every field is checked."""
    fixture = "/tmp/itchbook_trace_fixture.bin"
    if not os.path.exists(fixture):
        check(False, "fixture missing: build/test_tick_to_trade must run first")
        return
    blob = open(fixture, "rb").read()
    magic, ver = struct.unpack_from("<II", blob, 0)
    check(magic == MAGIC and ver == VERSION, "the fixture's magic and version match")
    off, seen = 8, {}
    while off + 12 <= len(blob):
        tag = blob[off:off + 4].decode()
        n, rec = struct.unpack_from("<II", blob, off + 4)
        off += 12
        seen[tag] = (n, rec, blob[off:off + n * rec])
        off += n * rec

    # Field-for-field. write_fixture() sets every field to a distinct number, so
    # ANY reordering or width change inside a struct shows up here.
    expect = {
        "CHNA": (CHAINA, (1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                          12, 13, 14, 15, 16, 17, ord('A'), 31, 2, 0)),
        "FILL": (FILLREC, (21, 22, 23, 24, 25, 1, 1, 0)),
        "ACPT": (ACCEPTEX, (31, 32, 33, ord('J'))),
        "EMIT": (EMITEX, (41, 42, 43, 44, 45)),
        "PKTS": (PKTEX, (51, 52, 53, 0)),
    }
    for tag, (fmt, want) in expect.items():
        if tag not in seen:
            check(False, "%s section present in the fixture" % tag)
            continue
        n, rec, body = seen[tag]
        check(rec == struct.calcsize(fmt),
              "%s record is %d bytes in C++ and %d here" % (tag, rec, struct.calcsize(fmt)))
        if rec != struct.calcsize(fmt):
            continue
        got = struct.unpack_from(fmt, body, 0)
        check(got == want,
              "%s round-trips EVERY field in order (got %s)"
              % (tag, "" if got == want else got))

    # The fixture must be from THIS build, not left over from a previous session.
    st = os.stat(fixture)
    exe = os.path.join(ROOT, "build", "test_tick_to_trade")
    if os.path.exists(exe):
        check(st.st_mtime >= os.stat(exe).st_mtime,
              "the fixture is newer than the binary that writes it, so it is "
              "not a stale file from an earlier session")


def main():
    print("=== the report's refusals, on inputs whose answers are known ===")
    for t in (test_clean_run_joins_the_right_records,
              test_a_mispaired_join_is_visible,
              test_negative_intra_process_hop_is_fatal,
              test_exchange_record_with_no_chain_is_fatal,
              test_duplicate_token_is_fatal,
              test_fill_the_exchange_never_saw_is_fatal,
              test_census_tags_only_the_chains_that_stopped,
              test_census_absent_means_no_p999,
              test_fixture_round_trips_from_cpp):
        print("\n%s" % t.__name__)
        t()
    print()
    if failures:
        print("test_report_join: %d failure(s)" % len(failures))
        for f in failures:
            print("  - %s" % f)
        return 1
    print("test_report_join: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
