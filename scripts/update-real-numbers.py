#!/usr/bin/env python3
"""Fold a real-data run's output back into the docs, without retyping it.

    ./scripts/real-data-run.sh ~/Desktop/12302019.NASDAQ_ITCH50.gz MSFT 200
    python3 scripts/update-real-numbers.py --out out/real --symbol MSFT

Three rounds of auditing this repository found the same defect over and over:
a number in a document that no artifact supported, or that an artifact
contradicted. Every one of them entered the same way — a human read a number
off a terminal and typed it into Markdown, and then the run was repeated at a
different sample size and the Markdown was not.

So this reads the run's own output files and rewrites the tables from them.
It refuses rather than guesses: if it cannot find a table it expects, it says
which file and exits non-zero, because a doc updater that silently does nothing
recreates the problem it exists to solve.

  --check   parse and report what WOULD change, touch nothing. Use this in CI
            or before committing.

It also refuses to write a SYNTHETIC run into a document about a real day.
docs/phase6-results.md section 1 is explicitly the real-data section, and
pointing this script at out/ after a generated-feed run would overwrite it with
numbers from a feed nobody claims anything about — which is the same defect in
the opposite direction. A real symbol-day is over a million messages; the
threshold is 500,000 and --force overrides it for the case where you genuinely
mean it.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MODELS = ("naive", "optimistic", "mbo", "pessimistic")


def parse_oracle(text):
    """The leave-one-out table: model / mean error / mean |error| / over / under / exact."""
    rows = {}
    for m in MODELS:
        hit = re.search(rf"^{m}\s+(-?[\d.]+)\s+([\d.]+)\s+(\d+)\s+(\d+)\s+(\d+)\s*$",
                        text, re.M)
        if hit:
            rows[m] = dict(zip(("mean", "abs", "over", "under", "exact"), hit.groups()))
    br = re.search(r"bracketed by \[pessimistic, optimistic\]:\s*(\d+)/(\d+)", text)
    n = re.search(r"(\d[\d,]*)\s+of the graded orders were pulled part-filled", text)
    msgs = re.search(r"^([\d,]+) messages,", text, re.M)
    if not rows or not br:
        raise SystemExit("error: no leave-one-out table found in the oracle output.\n"
                         "       Did real-data-run.sh get as far as the external check?")
    return {"rows": rows, "bracketed": int(br.group(1)), "n": int(br.group(2)),
            "partial": n.group(1) if n else None,
            "messages": int(msgs.group(1).replace(",", "")) if msgs else 0}


def parse_adversarial(text):
    """The scenario matrix and its tally."""
    tally = re.search(r"CORRECT=(\d+)\s+SAFE=(\d+)", text)
    cautious = re.search(r"CAUTIOUS=(\d+)", text)
    rows = re.findall(r"^([a-z0-9-]+)\s+(CORRECT|SAFE|CAUTIOUS|WRONG)\s+([\d,]+)\s+(\d+)",
                      text, re.M)
    if not tally or not rows:
        raise SystemExit("error: no scenario matrix found in the adversarial output.")
    return {"correct": int(tally.group(1)), "safe": int(tally.group(2)),
            "cautious": int(cautious.group(1)) if cautious else 0,
            "rows": rows}


def render_oracle_table(o):
    lines = ["| model | mean error | mean abs error | over | under | exact |",
             "|---|---:|---:|---:|---:|---:|"]
    for m in MODELS:
        r = o["rows"].get(m)
        if r is None:
            continue
        mean = r["mean"]
        if not mean.startswith("-") and float(mean) != 0:
            mean = "+" + mean
        exact = f"**{r['exact']}**" if m == "mbo" else r["exact"]
        lines.append(f"| {m} | {mean} | {r['abs']} | {r['over']} | {r['under']} | {exact} |")
    return "\n".join(lines)


def replace_block(path, start_pat, end_pat, new_block, label):
    """Swap the lines between two anchors. Refuses if the anchors are not found."""
    text = path.read_text()
    m0 = re.search(start_pat, text, re.M)
    if not m0:
        raise SystemExit(f"error: could not find the {label} anchor in {path}.\n"
                         f"       Pattern: {start_pat}")
    m1 = re.search(end_pat, text[m0.end():], re.M)
    if not m1:
        raise SystemExit(f"error: found the start of {label} in {path} but not its end.")
    lo, hi = m0.end(), m0.end() + m1.start()
    old = text[lo:hi]
    if old.strip() == new_block.strip():
        return text, False
    return text[:lo] + "\n" + new_block + "\n" + text[hi:], True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="out/real", help="real-data-run.sh output dir")
    ap.add_argument("--symbol", default="MSFT")
    ap.add_argument("--check", action="store_true",
                    help="report what would change and exit non-zero if anything would")
    ap.add_argument("--force", action="store_true",
                    help="write even if the run looks synthetic (see the size guard)")
    a = ap.parse_args()

    out = Path(a.out)
    oracle_f = out / f"{a.symbol}-oracle.txt"
    adv_f = out / f"{a.symbol}-adversarial.txt"
    for f in (oracle_f, adv_f):
        if not f.exists():
            raise SystemExit(f"error: {f} not found. Run:\n"
                             f"       ./scripts/real-data-run.sh <day>.gz {a.symbol} 200")

    o = parse_oracle(oracle_f.read_text())
    adv = parse_adversarial(adv_f.read_text())

    # The document this writes into is titled "On real data". A generated feed
    # is a few tens of thousands of messages; one real symbol-day is over a
    # million. Writing the former into the latter is the same defect as any
    # other unsupported number, just pointed the other way.
    MIN_REAL = 500_000
    if o["messages"] < MIN_REAL and not a.force:
        raise SystemExit(
            f"error: this run covers {o['messages']:,} messages, which is far short of\n"
            f"       a real trading day ({MIN_REAL:,}+). docs/phase6-results.md section 1\n"
            f"       is the REAL-data section; writing a generated run into it would put\n"
            f"       numbers there that nothing claims anything about.\n"
            f"       Pass --force if you genuinely mean to.")

    print(f"leave-one-out: {o['bracketed']}/{o['n']} bracketed")
    for m in MODELS:
        r = o["rows"].get(m)
        if r:
            print(f"  {m:<12} mean {r['mean']:>7}  over {r['over']:>4}  "
                  f"under {r['under']:>4}  exact {r['exact']:>4}")
    print(f"adversarial: {len(adv['rows'])} scenarios, "
          f"CORRECT={adv['correct']} SAFE={adv['safe']} CAUTIOUS={adv['cautious']}")

    p6 = ROOT / "docs" / "phase6-results.md"
    new_text, changed = replace_block(
        p6,
        r"^\| model \| mean error \| mean abs error \| over \| under \| exact \|\n\|---\|---:\|---:\|---:\|---:\|---:\|\n",
        r"^\s*$",
        render_oracle_table(o),
        "leave-one-out table")

    if a.check:
        print("\n" + ("phase6-results.md table is STALE — rerun without --check"
                      if changed else "phase6-results.md table matches the run."))
        return 1 if changed else 0

    if changed:
        p6.write_text(new_text)
        print(f"\nupdated {p6.relative_to(ROOT)}")
    else:
        print("\nphase6-results.md already matches the run.")

    print("\nStill to do by hand, because they are prose rather than a table:")
    print(f"  - README's leave-one-out paragraph: {o['bracketed']}/{o['n']} bracketed, "
          f"naive over {o['rows']['naive']['over']}, "
          f"pessimistic under {o['rows']['pessimistic']['under']}")
    print(f"  - README's adversarial block and phase7 section 9: "
          f"{len(adv['rows'])} rows, CORRECT={adv['correct']} SAFE={adv['safe']}")
    print(f"  - the full run output is in {out}/ if you want to paste a table verbatim")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
