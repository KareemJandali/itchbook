#!/usr/bin/env python3
"""Render docs/paper/as-on-itch.md as one self-contained HTML page.

    python3 scripts/paper-html.py           # write it
    python3 scripts/paper-html.py --check   # fail if it is stale

The Markdown is the source of truth and this is its build output, so it gets the
same treatment as every other generated document here: CI runs --check, and the
page cannot drift from the paper the way a hand-exported PDF does.

Stdlib only, like scripts/render-writeup.py, and for the same reason — a build
step that needs pandoc is a build step that works on one machine. It handles
exactly the constructs this paper uses: three heading levels, paragraphs,
bullet lists, tables, fenced code, block quotes, rules, images, and inline
code/strong/em/links. Anything else needs extending rather than guessing.

FIGURES ARE INLINED, not linked. A paper that is one file survives being emailed,
and an <img src="../figures/..."> does not survive being printed from a browser
that has been handed only the HTML. A missing figure is a hard error here rather
than a broken-image icon: scripts/paper-figures.sh guarantees a figure exists iff
its artifact does, so a dangling link means that guarantee has been broken.
"""
import argparse
import html
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "docs" / "paper" / "as-on-itch.md"
OUT = ROOT / "docs" / "paper" / "as-on-itch.html"

CSS = """
:root { --ink:#16181d; --mute:#5b6270; --rule:#d8dce3; --accent:#1f4e79;
        --code-bg:#f5f6f8; }
* { box-sizing: border-box; }
body { margin: 0 auto; padding: 3rem 1.5rem 6rem; max-width: 46rem;
       font: 16px/1.65 Georgia, 'Iowan Old Style', 'Times New Roman', serif;
       color: var(--ink); background: #fff; }
h1 { font-size: 1.9rem; line-height: 1.25; margin: 0 0 .4rem; letter-spacing: -.01em; }
h2 { font-size: 1.3rem; margin: 2.6rem 0 .8rem; padding-top: .8rem;
     border-top: 1px solid var(--rule); }
h3 { font-size: 1.05rem; margin: 1.8rem 0 .6rem; color: var(--accent); }
p, ul, ol { margin: 0 0 1rem; }
ul { padding-left: 1.3rem; }
li { margin: .35rem 0; }
em { font-style: italic; }
a { color: var(--accent); }
hr { border: 0; border-top: 1px solid var(--rule); margin: 2rem 0; }
hr + h2 { border-top: 0; padding-top: 0; margin-top: 1.4rem; }
code { font: .87em/1.5 ui-monospace, 'SF Mono', Menlo, Consolas, monospace;
       background: var(--code-bg); padding: .1em .3em; border-radius: 3px; }
pre { background: var(--code-bg); border: 1px solid var(--rule); border-radius: 4px;
      padding: .8rem 1rem; overflow-x: auto; }
pre code { background: none; padding: 0; font-size: .82rem; }
blockquote { margin: 1.2rem 0; padding: .8rem 1.1rem; border-left: 3px solid var(--accent);
             background: #f7f9fc; color: var(--ink); }
blockquote > :last-child { margin-bottom: 0; }
table { border-collapse: collapse; width: 100%; margin: 1rem 0 1.4rem;
        font: 13px/1.45 ui-monospace, 'SF Mono', Menlo, Consolas, monospace; }
th, td { border-bottom: 1px solid var(--rule); padding: .35rem .5rem; text-align: left; }
th { border-bottom: 2px solid var(--ink); font-weight: 700; white-space: nowrap; }
td.num, th.num { text-align: right; }
figure { margin: 1.6rem 0; }
figure svg { max-width: 100%; height: auto; display: block; }
figcaption { font-size: .85rem; color: var(--mute); margin-top: .5rem; font-style: italic; }
.byline { color: var(--mute); font-style: italic; margin: 0 0 2rem; }
@media print {
  body { padding: 0; max-width: none; font-size: 10.5pt; }
  h2, h3 { break-after: avoid; }
  table, figure, pre, blockquote { break-inside: avoid; }
  a { color: inherit; text-decoration: none; }
}
@page { margin: 18mm 16mm; }
"""

COMMENT = re.compile(r"^\s*<!--.*-->\s*$")
FENCE = re.compile(r"^```")
HEAD = re.compile(r"^(#{1,3})\s+(.*)$")
ROW = re.compile(r"^\|.*\|\s*$")
SEP = re.compile(r"^\|[\s:|-]+\|\s*$")
BULLET = re.compile(r"^[-*]\s+(.*)$")


def inline(t):
    """Inline spans. Code first, held out of everything downstream."""
    held = []

    def hold(m):
        held.append(f"<code>{html.escape(m.group(1))}</code>")
        return f"\x00{len(held) - 1}\x00"

    t = re.sub(r"`([^`]+)`", hold, t)
    t = html.escape(t, quote=False)
    t = re.sub(r"!\[([^\]]*)\]\(([^)]+)\)", r"<!--img:\2:\1-->", t)
    t = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2">\1</a>', t)
    t = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", t)
    t = re.sub(r"(?<!\*)\*([^*]+)\*(?!\*)", r"<em>\1</em>", t)
    t = t.replace(r"\|", "|")
    return re.sub(r"\x00(\d+)\x00", lambda m: held[int(m.group(1))], t)


def figure(src, alt, caption):
    """Inline the SVG. A missing one is an error, not a broken image."""
    p = (SRC.parent / src).resolve()
    if not p.exists():
        sys.exit(f"{SRC.name} links {src}, which does not exist. "
                 f"Run scripts/paper-figures.sh — and if it says the artifact is "
                 f"missing, the link should not be in the paper at all.")
    svg = p.read_text(encoding="utf-8")
    svg = svg[svg.index("<svg"):]                       # drop any XML prolog
    cap = f"<figcaption>{caption}</figcaption>" if caption else ""
    return f'<figure role="img" aria-label="{html.escape(alt, quote=True)}">{svg}{cap}</figure>'


def render(lines, depth=0):
    out = []
    i = 0
    n = len(lines)
    while i < n:
        ln = lines[i]

        if COMMENT.match(ln) or not ln.strip():
            i += 1
            continue

        if FENCE.match(ln):
            i += 1
            buf = []
            while i < n and not FENCE.match(lines[i]):
                buf.append(html.escape(lines[i]))
                i += 1
            i += 1
            out.append("<pre><code>" + "\n".join(buf) + "</code></pre>")
            continue

        m = HEAD.match(ln)
        if m:
            lvl = len(m.group(1))
            out.append(f"<h{lvl}>{inline(m.group(2))}</h{lvl}>")
            i += 1
            continue

        if ln.startswith(">"):
            buf = []
            while i < n and lines[i].startswith(">"):
                buf.append(lines[i][1:].lstrip(" ") if len(lines[i]) > 1 else "")
                i += 1
            out.append("<blockquote>" + render(buf, depth + 1) + "</blockquote>")
            continue

        if ROW.match(ln) and i + 1 < n and SEP.match(lines[i + 1]):
            def cells(row):
                # Split on unescaped pipes only.
                body = row.strip().strip("|")
                parts = re.split(r"(?<!\\)\|", body)
                return [c.strip() for c in parts]

            head = cells(ln)
            align = [("num" if c.strip().endswith(":") and not c.strip().startswith(":")
                      else "") for c in cells(lines[i + 1])]
            i += 2
            body = []
            while i < n and ROW.match(lines[i]):
                body.append(cells(lines[i]))
                i += 1
            th = "".join(f'<th class="{a}">{inline(c)}</th>'
                         for c, a in zip(head, align + [""] * len(head)))
            trs = []
            for r in body:
                tds = "".join(f'<td class="{a}">{inline(c)}</td>'
                              for c, a in zip(r, align + [""] * len(r)))
                trs.append(f"<tr>{tds}</tr>")
            out.append(f"<table><thead><tr>{th}</tr></thead><tbody>"
                       + "".join(trs) + "</tbody></table>")
            continue

        if BULLET.match(ln):
            items = []
            while i < n:
                b = BULLET.match(lines[i])
                if b:
                    items.append([b.group(1)])
                    i += 1
                elif items and lines[i].startswith("  ") and lines[i].strip():
                    items[-1].append(lines[i].strip())
                    i += 1
                else:
                    break
            out.append("<ul>" + "".join(f"<li>{inline(' '.join(it))}</li>"
                                        for it in items) + "</ul>")
            continue

        if ln.strip() == "---":
            out.append("<hr>")
            i += 1
            continue

        para = [ln]
        i += 1
        while i < n and lines[i].strip() and not (
                HEAD.match(lines[i]) or FENCE.match(lines[i]) or ROW.match(lines[i])
                or BULLET.match(lines[i]) or lines[i].startswith(">")
                or lines[i].strip() == "---" or COMMENT.match(lines[i])):
            para.append(lines[i])
            i += 1
        text = inline(" ".join(x.strip() for x in para))

        # A paragraph that is only an image becomes a figure, and the italic
        # line that follows it in the Markdown becomes its caption.
        only = re.fullmatch(r"<!--img:([^:]+):([^>]*)-->", text.strip())
        if only:
            cap = ""
            j = i
            while j < n and not lines[j].strip():
                j += 1
            if j < n and lines[j].strip().startswith("*") and lines[j].strip().endswith("*"):
                cap = inline(lines[j].strip())
                cap = re.sub(r"^<em>|</em>$", "", cap)
                i = j + 1
            out.append(figure(only.group(1), only.group(2), cap))
            continue
        out.append(f"<p>{text}</p>")
    return "\n".join(out)


def build():
    lines = SRC.read_text(encoding="utf-8").split("\n")
    # The first heading is the title; the italic line under it is the byline.
    title = next(l[2:].strip() for l in lines if l.startswith("# "))
    ti = lines.index(f"# {title}")
    byline = ""
    if ti + 2 < len(lines) and lines[ti + 2].startswith("*"):
        byline = inline(lines[ti + 2].strip())
        byline = re.sub(r"^<em>|</em>$", "", byline)
        del lines[ti + 2]
    del lines[ti]

    body = render(lines)
    if "<!--img:" in body:
        sys.exit("an image reference survived rendering; the paper uses a "
                 "construct this renderer does not handle")
    return ("<!doctype html>\n<html lang=\"en\">\n<head>\n"
            "<meta charset=\"utf-8\">\n"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
            f"<title>{html.escape(title)}</title>\n<style>{CSS}</style>\n"
            "</head>\n<body>\n"
            f"<h1>{inline(title)}</h1>\n"
            + (f'<p class="byline">{byline}</p>\n' if byline else "")
            + body + "\n</body>\n</html>\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()
    page = build()
    if a.check:
        if not OUT.exists() or OUT.read_text(encoding="utf-8") != page:
            print(f"{OUT.relative_to(ROOT)} is stale. "
                  f"Run: python3 scripts/paper-html.py", file=sys.stderr)
            return 1
        print("paper HTML matches the Markdown")
        return 0
    # newline="\n" or this writes CRLF on Windows into a tracked file, which is
    # the failure .gitattributes exists to prevent -- and unlike the two gates
    # above, which merely reported staleness, this one silently rewrote 523 line
    # endings in docs/paper/as-on-itch.html the first time it was run there.
    OUT.write_text(page, encoding="utf-8", newline="\n")
    print(f"wrote {OUT.relative_to(ROOT)} ({len(page):,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
