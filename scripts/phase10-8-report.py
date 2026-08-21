#!/usr/bin/env python3
"""Generate the reader-overlap tables in docs/phase10-results.md.

    python3 scripts/phase10-8-report.py            # write it
    python3 scripts/phase10-8-report.py --check    # fail if it is stale

Standing rule 7. Also grades the predictions in docs/build-plan-9-12.md, which
were committed before the reader thread was run once -- and grades them by
computing the verdict rather than by having one typed, because phase 9's report
script spent a while cheerfully printing "kept" after the numbers had moved
outside the predicted range.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ART = ROOT / "validation" / "reader-overlap.json"
OUT = ROOT / "docs" / "phase10-results.md"
BEGIN = "<!-- generated:overlap:begin -->"
END = "<!-- generated:overlap:end -->"

# The predicted ranges, transcribed once from the committed plan. A verdict is
# computed against these; none is written by hand.
P2_RANGE = (1.2, 1.7)          # speedup in this container
P3_THRESHOLD = 0.95            # overlapped >= this x sequential means refuted
P4_FLAT_PERCENT = 10.0         # chunk-size spread below this counts as flat


def build(d):
    L = [BEGIN, "", "## 10.8 — decompressing on its own thread", "",
         "The prediction was that the ceiling is arithmetic: decompression costs "
         "D, the book costs B, `parse()` alternates between them on one thread, "
         "so the sequential path pays D + B and the overlapped path cannot beat "
         "max(D, B). Available speedup (D + B) / max(D, B), at most 2×.", "",
         "**The measurement went straight through that ceiling** — at every "
         "chunk size, by 24% to 37%. A pipeline cannot beat max(D, B), so the "
         "fault is in the decomposition, and the model's hidden assumption is "
         "the culprit: that the work is *invariant under the split*. It is not.", "",
         "| | |", "|---|---:|",
         f"| feed | {d['feed_messages']:,} messages |",
         f"| D — decompress, frame, length-check, build nothing | {d['decompress_seconds']:.2f} s |",
         f"| B — the book, by subtraction (T_seq − D) | {d['book_seconds']:.2f} s |",
         f"| D + B — the single-threaded path | {d['sequential_seconds']:.2f} s |",
         f"| model ceiling (D + B) / max(D, B) | {d['ceiling']:.2f}× |",
         f"| larger half | {d['bound_by']} |",
         f"| runs per configuration | {d['repeats']} (best of) |",
         "",
         "### Why the ceiling leaks, measured", "",
         "`gzread` reads an uncompressed file transparently, so the same "
         "binaries run on the same bytes with inflate taken out of the picture. "
         "That separates *what the book costs* from *what the book appears to "
         "cost while interleaved with inflate*.", "",
         "| | |", "|---|---:|",
         f"| frame only, no inflate | {d['frame_only_no_inflate_seconds']:.2f} s |",
         f"| frame + book, no inflate | {d['frame_and_book_no_inflate_seconds']:.2f} s |",
         f"| **B isolated** | **{d['book_isolated_seconds']:.2f} s** |",
         f"| subtraction overstates the book by | "
         f"{d['subtraction_overstates_book_percent']:.0f}% |",
         "",
         "Two effects, both real, and neither one is overlap:", "",
         "1. **Inflate and the book contend.** B by subtraction is "
         f"{d['book_seconds']:.2f} s; the book's isolated cost is "
         f"{d['book_isolated_seconds']:.2f} s. zlib's 32 KB window and the "
         "book's ref map do not fit in the same cache together, so interleaving "
         "them on one core costs more than running either alone.", "",
         "2. **The split moves work off the consumer.** Framing alone is "
         f"{d['frame_only_no_inflate_seconds']:.2f} s for "
         f"{d['feed_messages']:,} messages — two `gzread` calls and a vector "
         "resize each. In the pipeline the producer absorbs all of it and the "
         "consumer walks a contiguous chunk instead. That is a cheaper inner "
         "loop, not overlap, and it lands in the same number.", ""]

    L += ["Stall columns are **poll counts, not time**, and are not comparable "
          "across the two sides: a consumer's empty poll is a load and a "
          "compare, while a producer's full poll goes through `writable()`, "
          "which refreshes the consumer's cache line. Which half is slower is "
          "settled above, by time.", "",
          "| chunk | wall clock | speedup | % of ceiling | producer polls | "
          "consumer polls | chunks |",
          "|---:|---:|---:|---:|---:|---:|---:|"]
    for c in d["chunks"]:
        L.append(f"| {c['chunk_kb']} KB | {c['elapsed_seconds']:.2f} s | "
                 f"{c['speedup']:.2f}× | {c['fraction_of_ceiling'] * 100:.0f}% | "
                 f"{c['producer_stalls']:,} | {c['consumer_stalls']:,} | "
                 f"{c['reader_chunks']:,} |")
    L.append("")

    # ---- the verdicts, computed ------------------------------------------
    best = d["best_speedup"]
    L += ["### The predictions, graded", "", "| | predicted | measured | verdict |",
          "|---|---|---|:--|"]

    L.append(f"| P1 the ceiling is arithmetic | measured speedup ≤ (D+B)/max(D,B) | "
             f"ceiling {d['ceiling']:.2f}×, best {best:.2f}× "
             f"({best / d['ceiling'] * 100:.0f}%) | "
             + ("**kept**" if not d["exceeds_model_ceiling"] else
                "**falsified — the model assumed the work is invariant under the "
                "split, and it is not**") + " |")

    lo, hi = P2_RANGE
    if best < lo:
        v2 = "**falsified — below the range**"
    elif best > hi:
        v2 = "**falsified — above the range**"
    else:
        v2 = "**kept**, though for partly the wrong reason: see P1"
    L.append(f"| P2 speedup here | {lo:g}–{hi:g}× | {best:.2f}× at "
             f"{d['best_chunk_kb']} KB | {v2} |")

    ratio = min(c["elapsed_seconds"] for c in d["chunks"]) / d["sequential_seconds"]
    L.append(f"| P3 falsification | refuted if overlapped ≥ {P3_THRESHOLD:g}× sequential | "
             f"{ratio:.2f}× sequential | "
             f"{'**REFUTED — the ring is decoration**' if d['overlap_refuted'] else '**holds**'} |")

    # Flatness by a single threshold would be decided by a coin flip here: two
    # runs of this sweep measured 10.9% and 7.6% spread against a 10% bar. What
    # is stable across both is the ORDER -- bigger chunks were faster every
    # time, at every size -- so the verdict reports both and claims neither more
    # than the data supports.
    times = [c["elapsed_seconds"] for c in d["chunks"]]
    monotonic = all(times[i] >= times[i + 1] for i in range(len(times) - 1))
    flat = abs(d["chunk_spread_percent"]) < P4_FLAT_PERCENT
    if flat and monotonic:
        v4 = ("**undecided** — spread is within the noise of this machine, but "
              "bigger chunks were faster at every size and in both runs of the "
              "sweep. A real effect too small to separate here")
    elif flat:
        v4 = "**kept — flat**"
    else:
        v4 = "**falsified — chunk size matters**"
    L.append(f"| P4 chunk size | FLAT | {d['chunk_spread_percent']:.1f}% spread "
             f"across 64 KB / 256 KB / 1 MB"
             f"{', monotonic' if monotonic else ', not ordered'} | {v4} |")

    prod = d["chunks"][0]["producer_stalls"]
    cons = d["chunks"][0]["consumer_stalls"]
    L.append(f"| P5 which side stalls | producer more often (B > D) | "
             f"producer {prod:,}, consumer {cons:,} | "
             "**withdrawn — poll counts are not comparable across the sides; "
             "see above** |")
    L += ["", END]
    return "\n".join(L)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()
    if not ART.exists():
        sys.exit(f"error: missing artifact {ART}")
    block = build(json.loads(ART.read_text()))
    text = OUT.read_text()
    head, rest = text.split(BEGIN, 1)
    _, tail = rest.split(END, 1)
    updated = head + block + tail
    if args.check:
        if updated != text:
            print("docs/phase10-results.md is stale. "
                  "Run: python3 scripts/phase10-8-report.py")
            return 1
        print("phase 10.8 results match their artifacts")
        return 0
    OUT.write_text(updated)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
