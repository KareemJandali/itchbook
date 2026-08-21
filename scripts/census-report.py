#!/usr/bin/env python3
"""Write the census block in validation/README.md from the census itself.

    python3 scripts/census-report.py            # rewrite the block
    python3 scripts/census-report.py --check    # fail if the block is stale

Standing rule 7: no number reaches a document by being retyped. Three rounds of
auditing this repository found the same defect over and over — a figure in a
document that no artifact supported — and every one of them entered by a human
reading a terminal and typing into Markdown. So the block between the two
markers below is generated from validation/census-2019-12-30.json, which is the
census tool's own output, committed.

--check is what CI runs. It touches nothing and exits non-zero if the committed
prose has drifted from the committed data.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CENSUS = ROOT / "validation" / "census-2019-12-30.json"
FLOOR = ROOT / "validation" / "census-2019-12-30-framing.json"
README = ROOT / "validation" / "README.md"
BEGIN = "<!-- census:begin -->"
END = "<!-- census:end -->"

# A stub quote is an order parked where it will never fill, to satisfy a
# two-sided quoting obligation. Nothing on the wire flags one; what identifies
# it is a price nothing could trade at. Same constants the tool prints.
ABSURDLY_HIGH = 100_000.0
ABSURDLY_LOW = 0.01


def commas(n):
    return f"{n:,}"


def build(census):
    syms = census["symbols"]
    quoted = [s for s in syms if s["adds"] > 0]
    live = census["live_orders"]
    secs = census["elapsed_seconds"]
    raw = census["bytes"]
    gz = census["compressed_bytes"]

    stub_hi = sum(1 for s in quoted if s["max_price"] and s["max_price"] >= ABSURDLY_HIGH)
    stub_lo = sum(1 for s in quoted if s["min_price"] and s["min_price"] <= ABSURDLY_LOW)
    # Absent when the census predates the printed-price fields. Reported as
    # missing rather than as zero: "no symbol traded" and "this run could not
    # tell" are different claims.
    has_prints = any("prints" in s for s in syms)
    traded = sum(1 for s in syms if s.get("prints", 0) > 0) if has_prints else None
    undirectoried = [s for s in syms if not s["directoried"]]

    lines = [
        BEGIN,
        "",
        "| | |",
        "|---|---:|",
        f"| messages | {commas(census['messages'])} |",
        f"| uncompressed | {raw / 1e9:.2f} GB |",
        f"| on disk (gzip) | {gz / 1e9:.2f} GB — a {raw / gz:.2f}x ratio |",
        f"| locates seen | {commas(len(syms))} |",
        f"| with a stock directory entry | {commas(sum(1 for s in syms if s['directoried']))} |",
        f"| that ever quoted an order | {commas(len(quoted))} |",
        f"| that ever printed a trade | {commas(traded)} |" if traded is not None else "",
        f"| with a closing cross | {commas(sum(1 for s in syms if s['closing_crosses'] > 0))} |",
        "",
        "**Live orders, one shared reference space across every symbol.**",
        "",
        "| | |",
        "|---|---:|",
        f"| peak resting at once | **{commas(live['peak'])}** |",
        f"| resting at the close | {commas(live['final'])} |",
        f"| adds (`A`+`F`) | {commas(live['adds'])} |",
        f"| replaces (`U`) | {commas(live['replaces'])} |",
        f"| deletes (`D`) | {commas(live['deletes'])} |",
        f"| executions that emptied an order | {commas(live['full_executions'])} |",
        f"| executions that did not | {commas(live['partial_executions'])} |",
        f"| cancels that emptied an order | {commas(live['full_cancels'])} |",
        f"| cancels that did not | {commas(live['partial_cancels'])} |",
        f"| **references naming no live order** | **{commas(live['unknown_refs'])}** |",
        "",
        f"The pass took **{secs:.2f} s** — {raw / secs / 1e6:.0f} MB/s of feed, "
        f"{census['messages'] / secs / 1e6:.2f} M msg/s — and it was the "
        f"`{census['pass']}` pass.",
        "",
        "**Stub quotes, and why a symbol's quoted range sizes nothing.**",
        "",
        f"Of the {commas(len(quoted))} symbols that quoted at all, "
        f"**{commas(stub_hi)} ({stub_hi / len(quoted) * 100:.1f}%) posted an order at or above "
        f"${ABSURDLY_HIGH:,.0f}**, and **{commas(stub_lo)} ({stub_lo / len(quoted) * 100:.1f}%) "
        f"posted one at or below ${ABSURDLY_LOW:.2f}**. Those are orders parked where they will "
        "never fill, satisfying a two-sided quoting obligation. Nothing on the wire marks them; "
        "what identifies them is a price nothing could trade at.",
        "",
        "The consequence is concrete: the range of prices a symbol *quoted* spans almost the "
        "whole price axis for three symbols in four, so it cannot centre a dense band or predict "
        "which symbols will overflow one. The range it *printed* can, which is why the census "
        "records both — and no generated feed in this repository produces a stub quote, so "
        "nothing here would ever have shown it.",
        "",
    ]
    if undirectoried:
        which = ", ".join(f"`{s['locate']}` ({s['messages']} messages)" for s in undirectoried[:5])
        lines += [
            f"**{len(undirectoried)} locate with no directory entry:** {which}. That is the "
            "session itself — `S` system events and the market-wide `V` carry stock locate 0, "
            "and no `R` describes it. A message for an undirectoried locate is counted rather "
            "than ignored, because the benign explanation and a framing bug look identical "
            "until someone looks.",
            "",
        ]
    # The floor, if a framing-only pass was recorded. It is the interesting
    # one: this pass decompresses, frames and length-checks the whole file and
    # builds nothing, so nothing that replays the same file can be faster, and
    # the gap between it and any replay is what the book itself costs.
    if FLOOR.exists():
        floor = json.loads(FLOOR.read_text())
        fs = floor["elapsed_seconds"]
        lines += [
            "",
            "**The floor.** A pass that decompresses, frames and length-checks the file while "
            "building nothing:",
            "",
            "| pass | seconds | MB/s | M msg/s |",
            "|---|---:|---:|---:|",
            f"| `{floor['pass']}` | **{fs:.2f}** | {floor['bytes'] / fs / 1e6:.0f} | "
            f"{floor['messages'] / fs / 1e6:.2f} |",
            f"| `{census['pass']}` | {secs:.2f} | {raw / secs / 1e6:.0f} | "
            f"{census['messages'] / secs / 1e6:.2f} |",
            "",
            f"No replay of this file can beat {fs:.2f} s, and the difference between that and "
            "any end-to-end number is what the book costs. The second row prices the live-order "
            f"table on its own: **{secs - fs:.1f} s** for roughly "
            f"{(live['adds'] + live['replaces'] + live['deletes'] + live['full_executions'] + live['partial_executions'] + live['partial_cancels']) / 1e6:.0f}"
            " M hash operations against a table far larger than L2 — which is the same shape of "
            "work the book's reference map does, and the reason the multi-symbol throughput "
            "prediction in the build plan is what it is.",
        ]

    lines.append(END)
    # Joined verbatim: the blank lines are markdown, not slack. The block ends
    # AT the end marker with no trailing newline, so head + block + tail
    # reproduces the file exactly and --check can compare them.
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    args = ap.parse_args()

    if not CENSUS.exists():
        sys.exit(f"error: no census at {CENSUS}. Run itch_census --per-symbol against a real day.")
    census = json.loads(CENSUS.read_text())
    block = build(census)

    text = README.read_text()
    if BEGIN not in text or END not in text:
        sys.exit(f"error: {README} has no {BEGIN} / {END} markers to write between.")
    head, rest = text.split(BEGIN, 1)
    _, tail = rest.split(END, 1)
    updated = head + block + tail

    if args.check:
        if updated != text:
            print("validation/README.md is stale against validation/census-2019-12-30.json.")
            print("Run: python3 scripts/census-report.py")
            return 1
        print("census block matches the committed census")
        return 0

    if updated == text:
        print("census block already current")
        return 0
    README.write_text(updated)
    print(f"rewrote the census block in {README}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
