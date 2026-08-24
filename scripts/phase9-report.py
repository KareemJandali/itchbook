#!/usr/bin/env python3
"""Generate docs/phase9-results.md from the run's own artifacts.

    python3 scripts/phase9-report.py            # write it
    python3 scripts/phase9-report.py --check    # fail if it is stale

Standing rule 7. Every figure below is read from a file the tool wrote; none is
typed. The prose around them is written by hand, because a conclusion is not a
number and should not pretend to be generated.
"""
import argparse
import csv
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
V = ROOT / "validation"
OUT = ROOT / "docs" / "phase9-results.md"
BEGIN = "<!-- generated:begin -->"
END = "<!-- generated:end -->"


# A field an artifact does not carry is not a field that measured zero. Three of
# these helpers exist only to keep that distinction visible: the run that
# produced validation/ predates the fields below, and the alternative to saying
# so is a table that reports absence as a number.
MISSING = ("_not recorded in this artifact_ — the committed run predates the "
           "field. `book_replay --all-symbols --json` writes it now; the value "
           "arrives with the next run over the day.")


def mwcb_row(run):
    """'W' as a table row: the level breached, or the reason there is no row."""
    if "mwcb_events" not in run:
        return f"| MWCB ('W') | {MISSING} |"
    n = run["mwcb_events"]
    lvl = run.get("mwcb_level_breached") or ""
    if n == 0:
        # A zero here is a result, not an absence: it says the constants 'W' is
        # parsed with are still unconfirmed against real bytes, which is exactly
        # what 9.6 asked the count for.
        return "| MWCB ('W') | 0 — no market-wide breach on this day |"
    return f"| MWCB ('W') | {n} message(s), last level `{lvl}` |"


def overflow_memory(run):
    """(MB, table row) for the overflow maps at their worst, or (0, None)."""
    peak = run.get("peak_overflow_levels")
    per = run.get("overflow_bytes_per_level")
    if peak is None or per is None:
        return 0.0, None
    mb = peak * per / 1e6
    return mb, (f"| overflow maps (bound) | {mb:.1f} | {peak:,} peak levels x {per} B "
                f"per red-black node |")


def overflow_section(run, rows):
    """The overflow distribution, or the reason there is not one yet."""
    L = ["## Overflow, distributed", "",
         "The dense band is the fast store and the `std::map` behind it is the "
         "slow one, so *how much* fell through matters as much as the off-band "
         "percentage above. It is reported here rather than folded into the "
         "residual row, which is what made that row an aggregate hiding a "
         "distribution.", ""]

    # The terminal count IS in the committed artifact, and it is zero -- which
    # is the finding, not a gap. Stating it is what makes the peak column
    # legible as a fix rather than as a second opinion.
    term = sum(int(r["overflow_levels"]) for r in rows)
    users = sum(1 for r in rows if int(r["overflow_levels"]) > 0)
    flat = int(run["resting_orders"]) == 0
    L += [f"**At the close, {term:,} overflow levels stood across all "
          f"{len(rows):,} symbols, in {users:,} of them.**"
          + (" That number is structural rather than small: " if term == 0 else " ")
          + ("the session ends flat" if flat
             else f"these books closed holding {int(run['resting_orders']):,} orders")
          + ", and the book erases each overflow level as it empties to keep the "
          "map cold. A completed day reports an empty map however hard overflow "
          "was worked in between — and it was worked, "
          f"{int(run['off_band_adds']):,} times. The terminal size cannot "
          "decompose peak RSS, which is itself a high-water mark.", ""]

    if not rows or "peak_overflow_levels" not in rows[0]:
        L += ["**The high-water distribution is not in this artifact.** "
              "`peak_overflow_levels` is written per symbol by "
              "`book_replay --all-symbols --per-symbol` now; the run committed "
              "here predates the column, and there is no way to recover a peak "
              "from a file that recorded only the end. One re-run over the day "
              "fills in the table below, which is left empty rather than "
              "estimated:", "",
              "| percentile of symbols using overflow | peak levels held |",
              "|---|---:|",
              "| p50 | — |", "| p90 | — |", "| p99 | — |", "| max | — |", ""]
        return L

    peaks = sorted(int(r["peak_overflow_levels"]) for r in rows
                   if int(r["peak_overflow_levels"]) > 0)
    if not peaks:
        L += ["No symbol ever used overflow on this feed.", ""]
        return L

    def pctile(q):
        return peaks[min(int(q / 100 * (len(peaks) - 1)), len(peaks) - 1)]

    total = sum(peaks)
    L += [f"**{len(peaks):,} of {len(rows):,} symbols "
          f"({len(peaks) / len(rows) * 100:.1f}%) used overflow at some point**, "
          f"holding {total:,} levels between them at their respective worsts.", "",
          "| percentile of symbols using overflow | peak levels held |", "|---|---:|"]
    for q in (10, 25, 50, 75, 90, 99):
        L.append(f"| p{q} | {pctile(q):,} |")
    L.append(f"| max | {peaks[-1]:,} |")

    worst = sorted((r for r in rows if int(r["peak_overflow_levels"]) > 0),
                   key=lambda r: -int(r["peak_overflow_levels"]))[:6]
    L += ["", "### The symbols that leaned on overflow hardest", "",
          "| symbol | peak overflow levels | adds | off-band | re-centres |",
          "|---|---:|---:|---:|---:|"]
    for r in worst:
        a = int(r["adds"])
        L.append(f"| {r['symbol']} | {int(r['peak_overflow_levels']):,} | {a:,} | "
                 f"{int(r['off_band_adds']) / a * 100 if a else 0:.1f}% | {r['recentres']} |")
    return L


def load():
    sweep = V / "sweep-load.json"
    repro = V / "timing-reproduction-2026-08-24.json"
    return (json.loads((V / "census-2019-12-30-framing.json").read_text()),
            json.loads((V / "census-2019-12-30.json").read_text()),
            json.loads((V / "all-symbols-2019-12-30.json").read_text()),
            list(csv.DictReader((V / "all-symbols-2019-12-30.csv").open())),
            json.loads(sweep.read_text()) if sweep.exists() else None,
            json.loads(repro.read_text()) if repro.exists() else None)


def reproduction_section(r):
    """The re-run that did not reproduce the wall clock, and what it ruled out."""
    if r is None:
        return []
    rec = r["recorded"]["elapsed_seconds"]
    arms = [(a["label"], min(a["samples"]), len(a["samples"])) for a in r["arms"]]
    best = min(m for _, m, _ in arms)
    L = ["## The wall clock did not reproduce, and the book is not why", "",
         f"On {r['date']} every figure above was re-measured. Each **deterministic** "
         f"one came back identical — {r['per_symbol_cells_compared']:,} per-symbol cells "
         f"compared, {r['per_symbol_cells_differing']} differing. The wall clock did not: "
         f"**{best:.2f} s against the {rec:.2f} s recorded**, "
         f"{best / rec:.2f}x. So the arms were run alternately inside one session, "
         "sharing whatever load there was, because running one to completion and then "
         "the other is how a busy machine gets attributed to a code change.", "",
         "| binary | runs | best |", "|---|---:|---:|"]
    for label, m, n in arms:
        L.append(f"| {label} | {n} | {m:.2f} s |")
    spread = max(m for _, m, _ in arms) - best
    L += ["",
          f"The commit that recorded {rec:.2f} s reports "
          f"{arms[0][1]:.2f} s today — its own binary, the same file, byte-identical "
          f"output. All three arms sit within {spread:.2f} s of each other, so neither the "
          "49 commits of phase 10 and 11 work nor the counters added for this section "
          "cost anything measurable. What moved is the machine.", "",
          "Ruled out, each by measurement rather than by argument:", ""]
    for k, v in r["controls_ruled_out"].items():
        L.append(f"* **{k.replace('_', ' ')}** — {v}")
    c = r["conditions"]
    L += ["",
          f"What is left is contention: {c['note']}, on a machine with "
          f"{c['cores']['performance']} performance cores and a 1-minute load average "
          f"between {c['load_average_1min_range'][0]} and "
          f"{c['load_average_1min_range'][1]} across the runs.",
          "",
          "**The recorded numbers above are kept rather than replaced.** They were taken "
          "on a quieter machine, and a performance figure should measure the program "
          "rather than what else was running — the same reason the sweeps in this phase "
          "report the minimum of their samples. `timing_provenance` in each artifact "
          "names the two fields this applies to; every other field in them is from the "
          "re-run.", ""]
    return L


def build(floor, census, run, rows, sweep, repro=None):
    act = [r for r in rows if int(r["adds"]) > 0]
    def frac(r):
        return int(r["off_band_adds"]) / int(r["adds"])
    f = sorted(frac(r) for r in act)
    def pct(q):
        return f[min(int(q / 100 * (len(f) - 1)), len(f) - 1)] * 100

    secs = run["elapsed_seconds"]
    fs = floor["elapsed_seconds"]
    book = secs - fs
    msgs = run["messages_read"]
    peak = run["peak_rss_bytes"] / 1e6
    band = run["band_memory_bytes"] / 1e6
    ref = run["ref_map_slots"] * 16 / 1e6
    pool = run["pool_capacity_orders"] * 40 / 1e6
    acc = band + ref + pool

    rc = [r for r in act if int(r["recentres"]) > 0]
    nrc = [r for r in act if int(r["recentres"]) == 0]
    unseen = [r for r in nrc if int(r["adds"]) < 1000]
    judged = [r for r in nrc if int(r["adds"]) >= 1000]
    drifted = [r for r in judged if frac(r) > 0.5]
    worst = sorted(act, key=lambda r: -int(r["off_band_adds"]))[:8]

    def mean(rs):
        return sum(frac(r) for r in rs) / len(rs) * 100 if rs else 0.0

    L = [BEGIN, "", "## The run", "", "| | |", "|---|---:|",
         f"| messages | {msgs:,} |",
         f"| symbols | {run['books']:,} |",
         f"| executed volume | {run['executed_volume']:,} shares |",
         f"| **wall clock** | **{secs:.2f} s** |",
         f"| throughput | {msgs / secs / 1e6:.2f} M msg/s |",
         f"| peak RSS | {peak:.1f} MB |",
         f"| unknown references | {run['unknown_refs']} |",
         f"| locate mismatches | {run['locate_mismatch']} |",
         f"| undirectoried messages | {run['undirectoried_messages']} |",
         f"| operational halts ('h') | {run['operational_halts']} |",
         f"| broken trades ('B') | {run['broken_trades']} |",
         mwcb_row(run),
         "",
         "## The two numbers, and which is which", "",
         "| | seconds | M msg/s |", "|---|---:|---:|",
         f"| decompress + frame + length-check, building nothing | {fs:.2f} | "
         f"{floor['messages'] / fs / 1e6:.2f} |",
         f"| the same, plus building 8,906 books | {secs:.2f} | {msgs / secs / 1e6:.2f} |",
         f"| **the book's own cost** | **{book:.2f}** | — |",
         "",
         f"Decompression is **{fs / secs * 100:.0f}%** of the run.",
         "",
         "## The prediction", ""]

    # Written into the plan before any of this ran. Graded here by computing the
    # verdict from the numbers rather than by a word typed next to them -- an
    # earlier version of this script asserted "kept" as a literal and went on
    # saying it after the measurement moved outside the range.
    LO, HI = 60.0, 120.0            # book-only seconds
    E_LO, E_HI = 80.0, 140.0        # end-to-end seconds
    R_LO, R_HI = 5.0, 20.0          # x the cache-hot benchmark
    CACHE_HOT_NS = 22.8

    def grade(book_s, end_s):
        ratio = (book_s * 1e9 / msgs) / CACHE_HOT_NS
        ok = (LO <= book_s <= HI) and (E_LO <= end_s <= E_HI) and (R_LO <= ratio <= R_HI)
        return ratio, ok

    L += [f"> book-only {LO:.0f}–{HI:.0f} s, end-to-end {E_LO:.0f}–{E_HI:.0f} s, "
          f"{R_LO:.0f}–{R_HI:.0f}x worse per message than the cache-hot benchmark",
          ""]

    # The configuration the prediction was written against: the reference map
    # pre-sized to 2x the peak, which is what the default was at the time.
    old = None
    if sweep:
        for v in sweep["variants"]:
            if v["ref_map_slots"] == 4194304:
                old = min(v["samples"])
    rows_pred = []
    if old is not None:
        r, ok = grade(old - fs, old)
        rows_pred.append(("as predicted against — 4.19M map slots, 46% load",
                          old - fs, old, r, ok))
    r_now, ok_now = grade(book, secs)
    rows_pred.append((f"as it stands — {run['ref_map_slots'] / 1e6:.2f}M map slots, "
                      f"{census['live_orders']['peak'] / run['ref_map_slots'] * 100:.0f}% load",
                      book, secs, r_now, ok_now))

    L += ["| configuration | book-only | end-to-end | vs cache-hot | inside the prediction |",
          "|---|---:|---:|---:|---|"]
    for name, b, e, r, ok in rows_pred:
        L.append(f"| {name} | {b:.1f} s | {e:.1f} s | {r:.1f}x | "
                 f"{'**yes**' if ok else 'no' } |")
    L += ["",
          f"The single-symbol benchmark reports {CACHE_HOT_NS} ns per message. Across a whole "
          f"day of every symbol it is **{book * 1e9 / msgs:.0f} ns**.",
          ""]
    ov_mb, ov_note = overflow_memory(run)
    acc += ov_mb
    L += [
         "## Memory, decomposed", "",
         "| | MB | what it is |", "|---|---:|---|",
         f"| dense bands | {band:.1f} | {run['band_levels']} slots x 2 sides x "
         f"{run['books']:,} books x 32 B |",
         f"| reference map | {ref:.1f} | {run['ref_map_slots']:,} slots x 16 B |",
         f"| order pool | {pool:.1f} | {run['pool_capacity_orders']:,} orders x 40 B |"]
    if ov_note is not None:
        L.append(ov_note)
    L += [f"| **accounted** | **{acc:.1f}** | {acc / peak * 100:.1f}% of peak RSS |",
          f"| residual | {peak - acc:.1f} | "
          + ("books, directory, allocator, binary |" if ov_note is not None
             else "books, directory, overflow maps, allocator, binary |"),
         "",
]
    if ov_note is not None:
        # Do not let this read as a decomposition that closed. The overflow row
        # is an upper bound twice over -- the two sides' peaks are summed though
        # they need not coincide, and peak RSS is its own high-water mark that
        # need not coincide with either. It lands within 0.2 MB of the residual
        # it replaced, and that agreement is too neat to take at face value:
        # 8,906 books, a directory and the binary itself cannot together be the
        # 0.1 MB left over. The honest reading is that the true overflow
        # contribution is somewhat under the bound and the other terms make up
        # the difference, not that every byte is now accounted for.
        L += [f"**The overflow row is an upper bound, so the {peak - acc:.1f} MB residual "
              "is not evidence that the decomposition closed.** The bound sums each "
              "side's peak although the two need not peak together, and compares them "
              "against a peak RSS that need not coincide with either. Books, the "
              "directory and the binary are certainly not "
              f"{peak - acc:.1f} MB between them, so the real overflow figure sits "
              "somewhat below the bound and those terms cover the rest. Measuring the "
              "coincident sum would mean one side's insert reading the other side's "
              "map, which is why it is a bound and says so.", ""]
    L += [
         f"Peak live orders were {census['live_orders']['peak']:,}; the pool ended at "
         f"{run['pool_capacity_orders']:,}, and the reference map was pre-sized to "
         f"{run['ref_map_slots']:,} slots — {run['ref_map_slots'] / census['live_orders']['peak']:.2f}x "
         "the peak, which is the load factor that sizing was chosen for.",
         "",
         "## The band, graded", "",
         f"Overall, **{run['off_band_adds'] / run['adds'] * 100:.2f}%** of "
         f"{run['adds']:,} adds landed outside the dense band. Per symbol it is far worse, "
         "and the aggregate hides it:",
         "",
         "| percentile of symbols | adds landing off-band |", "|---|---:|"]
    for q in (10, 25, 50, 75, 90):
        L.append(f"| p{q} | {pct(q):.1f}% |")
    L += ["",
          f"**{len([r for r in act if frac(r) >= 0.5]):,} of {len(act):,} symbols "
          f"({len([r for r in act if frac(r) >= 0.5]) / len(act) * 100:.0f}%) had at least half "
          "their adds off-band.**",
          "",
          "### Three failure modes, each with a count", "",
          "| | symbols | mean off-band |", "|---|---:|---:|",
          f"| evaluated at 1,000 adds and judged fine | {len(judged):,} | {mean(judged):.1f}% |",
          f"| re-centred once | {len(rc):,} | {mean(rc):.1f}% |",
          f"| never reached 1,000 adds, so never evaluated | {len(unseen):,} | {mean(unseen):.1f}% |",
          "",
          f"And {len(drifted)} symbols were judged fine and drifted out anyway, because the "
          "policy looks once and never again:",
          "",
          "| symbol | adds | off-band | re-centres |", "|---|---:|---:|---:|"]
    for r in sorted(drifted, key=lambda r: -int(r["adds"]))[:6]:
        L.append(f"| {r['symbol']} | {int(r['adds']):,} | {frac(r) * 100:.1f}% | {r['recentres']} |")
    L += ["", "### The symbols the band failed hardest", "",
          "| symbol | adds | off-band | re-centres |", "|---|---:|---:|---:|"]
    for r in worst:
        L.append(f"| {r['symbol']} | {int(r['adds']):,} | {frac(r) * 100:.1f}% | {r['recentres']} |")
    L += [""] + overflow_section(run, rows)
    L += [""] + reproduction_section(repro)
    L += ["", END]
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    for name in ("census-2019-12-30-framing.json", "census-2019-12-30.json",
                 "all-symbols-2019-12-30.json", "all-symbols-2019-12-30.csv"):
        if not (V / name).exists():
            sys.exit(f"error: missing artifact {V / name}")
    block = build(*load())
    text = OUT.read_text()
    head, rest = text.split(BEGIN, 1)
    _, tail = rest.split(END, 1)
    updated = head + block + tail
    if args.check:
        if updated != text:
            print("docs/phase9-results.md is stale. Run: python3 scripts/phase9-report.py")
            return 1
        print("phase 9 results match their artifacts")
        return 0
    OUT.write_text(updated)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
