#!/usr/bin/env python3
"""reader-overlap.py — does decompressing on its own thread actually pay?

Phase 10.8. The obvious model says the ceiling is arithmetic: if decompression
costs D and the book costs B, the single-threaded path pays D + B because
`parse()` alternates between them on one thread, the overlapped path cannot beat
max(D, B), and so the available speedup is (D + B) / max(D, B) -- at most 2x,
and 2x only when the halves are balanced.

THAT MODEL IS WRONG, AND THIS SCRIPT MEASURES HOW. It was written expecting to
confirm the ceiling and instead recorded 1.65x, 1.73x and 1.83x against a
computed ceiling of 1.34x -- 124%, 129% and 137%, systematically and at every
chunk size. A pipeline cannot beat max(D, B), so the fault is in the
decomposition, and the model's hidden assumption is the culprit: that the WORK
IS INVARIANT under the split. It is not, for two measured reasons.

  1. B by subtraction (T_seq - D) overstates the book. Run the same feed
     uncompressed -- zlib reads a plain file transparently, so the same binaries
     work -- and the book's isolated cost comes out well below T_seq - D.
     Inflate and the book contend when interleaved on one core: zlib's window
     and the book's ref map do not fit together.
  2. The split MOVES work off the consumer. In the single-threaded path every
     message costs two `gzread` calls and a vector resize; in the pipeline the
     producer absorbs those and the consumer walks a contiguous chunk. That is
     not overlap, it is a cheaper inner loop, and it shows up in the same
     number.

So this measures four things rather than two: D (decompress + frame, from
itch_census), T_seq (the single-threaded path), and the same pair again on an
uncompressed copy of the feed, which isolates the book from inflate. The
"ceiling" is still reported, because a measurement above it is the signal that
the decomposition is leaking -- but it is reported as a model, not a bound.

Every timing is best-of-N. The noise here is one-sided: a descheduled thread or
a competing process can only make a run slower.

The predictions this grades are in docs/build-plan-9-12.md, written and
committed before the reader thread was run once.

Usage:
  bench/reader-overlap.py --build build --out validation/reader-overlap.json
                          [--feed f.gz] [--messages N] [--repeats N]
"""
import argparse
import gzip
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run_json(cmd, path):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{' '.join(cmd)} failed:\n{r.stdout}\n{r.stderr}")
    with open(path) as f:
        return json.load(f)


def best(runs, key):
    """Lowest wall clock wins, and the whole record comes with it.

    Taking the minimum of each field independently would produce a run that
    never happened -- the fastest elapsed paired with some other run's stall
    counts. The stall counters are only interpretable against the run that
    produced them.
    """
    return min(runs, key=lambda r: r[key])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build")
    ap.add_argument("--out", default="validation/reader-overlap.json")
    ap.add_argument("--feed")
    ap.add_argument("--messages", type=int, default=4000000)
    ap.add_argument("--symbols", type=int, default=512)
    ap.add_argument("--repeats", type=int, default=3)
    a = ap.parse_args()

    build = os.path.abspath(a.build)
    for t in ("book_replay", "itch_census"):
        if not os.path.exists(os.path.join(build, t)):
            sys.exit(f"missing {build}/{t}")

    work = tempfile.mkdtemp(prefix="reader-overlap-")
    try:
        feed = a.feed
        if feed is None:
            feed = os.path.join(work, "feed.gz")
            r = subprocess.run(
                [sys.executable, os.path.join(HERE, "scripts", "make-synthetic-feed.py"),
                 feed, "--messages", str(a.messages), "--symbols", str(a.symbols)],
                capture_output=True, text=True)
            if r.returncode != 0:
                sys.exit(r.stderr)
            print(r.stderr.strip(), file=sys.stderr)

        tj = os.path.join(work, "t.json")
        bj = os.path.join(work, "b.json")

        # D: the floor. A pass that decompresses, frames and length-checks the
        # file and builds nothing. No replay of the same file can be faster.
        census = [run_json([os.path.join(build, "itch_census"), feed, "--timing", tj], tj)
                  for _ in range(a.repeats)]
        D = min(c["elapsed_seconds"] for c in census)
        n_msgs = census[0]["messages"]

        # D + B: the single-threaded path every committed number came from.
        seq_runs = []
        for _ in range(a.repeats):
            run_json([os.path.join(build, "book_replay"), feed, "--all-symbols",
                      "--json", bj, "--quiet"], bj)
            seq_runs.append(json.load(open(bj)))
        seq = best(seq_runs, "elapsed_seconds")
        T_seq = seq["elapsed_seconds"]

        B = max(T_seq - D, 0.0)
        ceiling = T_seq / max(D, B) if max(D, B) > 0 else float("inf")

        # The same two measurements with inflate taken out of the picture, which
        # is what separates "the book costs B" from "the book appears to cost B
        # when it is interleaved with inflate". gzread reads an uncompressed
        # file transparently, so this is the same binaries on the same bytes.
        plain = os.path.join(work, "feed.raw")
        with gzip.open(feed, "rb") as src, open(plain, "wb") as dst:
            shutil.copyfileobj(src, dst, 1 << 20)
        D_plain = min(run_json([os.path.join(build, "itch_census"), plain,
                                "--timing", tj], tj)["elapsed_seconds"]
                      for _ in range(a.repeats))
        T_plain = float("inf")
        for _ in range(a.repeats):
            run_json([os.path.join(build, "book_replay"), plain, "--all-symbols",
                      "--json", bj, "--quiet"], bj)
            T_plain = min(T_plain, json.load(open(bj))["elapsed_seconds"])
        B_isolated = max(T_plain - D_plain, 0.0)
        os.remove(plain)

        print(f"feed: {n_msgs:,} messages")
        print(f"  D  decompress + frame only        {D:7.2f} s")
        print(f"  B  the book, by subtraction       {B:7.2f} s")
        print(f"  D+B single-threaded               {T_seq:7.2f} s")
        print(f"  model ceiling (D+B)/max(D,B)      {ceiling:7.2f}x  "
              f"[{'book' if B > D else 'decompression'}-bound]")
        print()
        print("  with inflate removed (same binaries, uncompressed copy):")
        print(f"  frame only, no inflate            {D_plain:7.2f} s")
        print(f"  frame + book, no inflate          {T_plain:7.2f} s")
        print(f"  B isolated                        {B_isolated:7.2f} s")
        if B > 0:
            print(f"  ...so subtraction overstates the book by "
                  f"{(B / B_isolated - 1) * 100 if B_isolated else 0:.0f}%")
        print()

        chunks = []
        # Stall columns are POLL counts and are not comparable across the two
        # sides -- a consumer's empty poll is far cheaper than a producer's full
        # one. Which half is slower is settled above, by time, from D and B.
        print(f"{'chunk':>8} {'wall s':>9} {'speedup':>9} {'% of ceiling':>13} "
              f"{'prod polls':>13} {'cons polls':>13}")
        for kb in (64, 256, 1024):
            runs = []
            for _ in range(a.repeats):
                run_json([os.path.join(build, "book_replay"), feed, "--all-symbols",
                          "--reader-thread", "--reader-chunk-kb", str(kb),
                          "--json", bj, "--quiet"], bj)
                runs.append(json.load(open(bj)))
            r = best(runs, "elapsed_seconds")
            t = r["elapsed_seconds"]
            sp = T_seq / t if t > 0 else 0.0
            row = {
                "chunk_kb": kb, "elapsed_seconds": t, "speedup": sp,
                "fraction_of_ceiling": sp / ceiling if ceiling else 0.0,
                "producer_stalls": r["producer_stalls"],
                "consumer_stalls": r["consumer_stalls"],
                "reader_chunks": r["reader_chunks"],
                "peak_occupancy": r["reader_peak_occupancy"],
                "messages_read": r["messages_read"],
                "all_runs": [x["elapsed_seconds"] for x in runs],
            }
            chunks.append(row)
            print(f"{kb:6d}KB {t:9.2f} {sp:8.2f}x {sp / ceiling * 100:12.0f}% "
                  f"{r['producer_stalls']:13,} {r['consumer_stalls']:13,}")

            if r["messages_read"] != seq["messages_read"]:
                sys.exit(f"the threaded path read {r['messages_read']} messages, "
                         f"the sequential one {seq['messages_read']}")

        fastest = max(chunks, key=lambda c: c["speedup"])
        spread = ((max(c["elapsed_seconds"] for c in chunks) /
                   min(c["elapsed_seconds"] for c in chunks)) - 1.0) * 100.0

        out = {
            "feed_messages": n_msgs, "repeats": a.repeats,
            "decompress_seconds": D, "book_seconds": B,
            "sequential_seconds": T_seq,
            "ceiling": ceiling,
            "frame_only_no_inflate_seconds": D_plain,
            "frame_and_book_no_inflate_seconds": T_plain,
            "book_isolated_seconds": B_isolated,
            "subtraction_overstates_book_percent":
                (B / B_isolated - 1) * 100 if B_isolated > 0 else None,
            "exceeds_model_ceiling": None,
            "bound_by": "book" if B > D else "decompression",
            "chunks": chunks,
            "best_speedup": fastest["speedup"],
            "best_chunk_kb": fastest["chunk_kb"],
            "chunk_spread_percent": spread,
            # P3: the falsification condition, evaluated rather than eyeballed.
            "overlap_refuted": fastest["elapsed_seconds"] >= 0.95 * T_seq,
        }
        out["exceeds_model_ceiling"] = out["best_speedup"] > ceiling * 1.02
        os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
        with open(a.out, "w") as f:
            json.dump(out, f, indent=1)

        print()
        print(f"best {fastest['speedup']:.2f}x at {fastest['chunk_kb']} KB, "
              f"{fastest['speedup'] / ceiling * 100:.0f}% of the {ceiling:.2f}x ceiling")
        print(f"chunk size spread across 64 KB / 256 KB / 1 MB: {spread:.1f}%")
        if out["exceeds_model_ceiling"]:
            print("\nThe best speedup EXCEEDS the model ceiling. That is not a faster-"
                  "than-light\npipeline, it is the decomposition leaking: the work is "
                  "not invariant under\nthe split. See the isolated-book numbers above "
                  "and docs/phase10-results.md.")
        if out["overlap_refuted"]:
            print("P3 FALSIFIED: the overlapped time is within 5% of the sequential "
                  "one.\nThe reader thread is not overlapping and the ring is decoration.")
        else:
            print("P3 holds: the overlapped path is more than 5% faster.")
        print(f"\nwrote {a.out}")
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
