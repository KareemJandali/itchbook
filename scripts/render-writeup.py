#!/usr/bin/env python3
"""Render the write-up as a standalone HTML page.

    python3 scripts/render-writeup.py

The Markdown is the source of truth; this exists so the posted version cannot
drift from it. Re-run after editing the essay and the page follows.

Deliberately stdlib-only, like everything else here — it handles exactly the
constructs this document uses (headings, one code fence, rules, inline
emphasis, one link) and would need extending for anything else.
"""
import html, re, sys
from pathlib import Path

SRC = Path('docs/writing/what-synthetic-data-hides.md')
OUT = Path('docs/writing/what-synthetic-data-hides.html')

def inline(t):
    t = html.escape(t, quote=False)
    t = re.sub(r'`([^`]+)`', r'<code>\1</code>', t)
    t = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', r'<a href="\2">\1</a>', t)
    t = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', t)
    t = re.sub(r'(?<!\*)\*([^*]+)\*(?!\*)', r'<em>\1</em>', t)
    return t

lines = SRC.read_text().split('\n')
body, i = [], 0
title = deck = None

while i < len(lines):
    ln = lines[i]

    if ln.startswith('```'):
        i += 1
        buf = []
        while i < len(lines) and not lines[i].startswith('```'):
            buf.append(html.escape(lines[i])); i += 1
        i += 1
        body.append('<pre><code>' + '\n'.join(buf) + '</code></pre>')
        continue

    if ln.strip() == '---':
        body.append('<hr>'); i += 1; continue

    if ln.startswith('# '):
        title = ln[2:].strip(); i += 1; continue

    if ln.startswith('## '):
        h = ln[3:].strip()
        m = re.match(r'^(\d+)\.\s+(.*)$', h)
        if m:
            body.append(f'<h2><span class="n">{m.group(1)}</span>'
                        f'<span class="t">{inline(m.group(2))}</span></h2>')
        else:
            body.append(f'<h2 class="coda"><span class="eyebrow">Conclusions</span>'
                        f'<span class="t">{inline(h)}</span></h2>')
        i += 1; continue

    if ln.strip() == '':
        i += 1; continue

    # paragraph: gather until blank
    buf = []
    while i < len(lines) and lines[i].strip() and not lines[i].startswith(('#', '```')) \
            and lines[i].strip() != '---':
        buf.append(lines[i].strip()); i += 1
    para = ' '.join(buf)
    if deck is None and para.startswith('*') and para.endswith('*'):
        deck = inline(para.strip('*')); continue
    cls = ''
    if para.startswith('*Source,'):
        cls = ' class="colophon"'
    body.append(f'<p{cls}>{inline(para)}</p>')

FONTS = ('https://fonts.googleapis.com/css2?'
         'family=IBM+Plex+Mono:wght@400;500&'
         'family=IBM+Plex+Sans+Condensed:wght@500;600;700&'
         'family=IBM+Plex+Serif:ital,wght@0,400;0,600;1,400&display=swap')

CSS = """
:root{
  --paper:#f7f8f9; --ink:#14181d; --muted:#5b6570; --rule:#d9dee3;
  --signal:#b8442a; --link:#2a5d9f; --wash:#eceff2; --deck:#3d4650;
}
@media (prefers-color-scheme:dark){
  :root:not([data-theme="light"]){
    --paper:#111417; --ink:#e6e9ec; --muted:#9aa4ae; --rule:#2a3037;
    --signal:#e2705a; --link:#7bb0ee; --wash:#1a1f24; --deck:#c3ccd4;
  }
}
:root[data-theme="dark"]{
  --paper:#111417; --ink:#e6e9ec; --muted:#9aa4ae; --rule:#2a3037;
  --signal:#e2705a; --link:#7bb0ee; --wash:#1a1f24; --deck:#c3ccd4;
}
*{box-sizing:border-box}
body{
  margin:0; background:var(--paper); color:var(--ink);
  font-family:"IBM Plex Serif",Georgia,"Times New Roman",serif;
  font-size:17px; line-height:1.68;
  -webkit-font-smoothing:antialiased;
}
.wrap{max-width:44rem; margin:0 auto; padding:clamp(2.5rem,6vw,5.5rem) 1.5rem 6rem}
header{border-bottom:2px solid var(--ink); padding-bottom:1.6rem; margin-bottom:2.8rem}
.kicker{
  font-family:"IBM Plex Mono",ui-monospace,monospace;
  font-size:.7rem; letter-spacing:.14em; text-transform:uppercase;
  color:var(--signal); margin:0 0 1rem;
}
h1{
  font-family:"IBM Plex Sans Condensed",system-ui,sans-serif;
  font-weight:700; font-size:clamp(2.3rem,7vw,3.6rem); line-height:1.02;
  letter-spacing:-.02em; margin:0; text-wrap:balance;
}
.deck{
  color:var(--deck); font-style:italic; font-size:1.06rem;
  margin:1.1rem 0 0; max-width:34rem;
}
h2{
  font-family:"IBM Plex Sans Condensed",system-ui,sans-serif;
  font-weight:600; font-size:1.5rem; line-height:1.18; letter-spacing:-.01em;
  margin:3.4rem 0 1.1rem; position:relative; text-wrap:balance;
}
h2 .n{
  font-family:"IBM Plex Mono",ui-monospace,monospace;
  font-size:.82rem; font-weight:500; color:var(--signal);
  display:block; margin-bottom:.45rem;
}
@media(min-width:60rem){
  h2 .n{position:absolute; left:-3.4rem; top:.42rem; margin:0; text-align:right; width:2.2rem}
}
h2.coda{border-top:1px solid var(--rule); padding-top:2.2rem}
.eyebrow{
  font-family:"IBM Plex Mono",ui-monospace,monospace;
  font-size:.7rem; letter-spacing:.14em; text-transform:uppercase;
  color:var(--muted); display:block; margin-bottom:.5rem; font-weight:400;
}
p{margin:0 0 1.15rem}
strong{font-weight:600}
code{
  font-family:"IBM Plex Mono",ui-monospace,monospace;
  font-size:.87em; background:var(--wash); padding:.1em .34em;
  border-radius:3px; overflow-wrap:break-word;
}
pre{
  background:var(--wash); border-left:2px solid var(--signal);
  padding:1rem 1.15rem; overflow-x:auto; margin:1.6rem 0;
  border-radius:0 4px 4px 0;
}
pre code{
  background:none; padding:0; font-size:.85rem; line-height:1.6;
  color:var(--ink); white-space:pre;
}
hr{border:0; border-top:1px solid var(--rule); margin:2.6rem 0}
h2 + hr, hr + hr{display:none}
a{color:var(--link); text-underline-offset:.18em; text-decoration-thickness:1px}
a:focus-visible{outline:2px solid var(--signal); outline-offset:3px; border-radius:2px}
.colophon{
  font-family:"IBM Plex Mono",ui-monospace,monospace;
  font-size:.82rem; color:var(--muted); line-height:1.6;
  border-top:1px solid var(--rule); padding-top:1.6rem; margin-top:2.6rem;
}
.colophon em{font-style:normal}
"""

page = (f'<title>What Synthetic Data Hides</title>\n'
        f'<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>\n'
        f'<link rel="stylesheet" href="{FONTS}">\n'
        f'<style>{CSS}</style>\n'
        f'<div class="wrap">\n<header>\n'
        f'<p class="kicker">Field notes &middot; NASDAQ TotalView-ITCH</p>\n'
        f'<h1>{html.escape(title)}</h1>\n'
        f'<p class="deck">{deck}</p>\n</header>\n'
        + '\n'.join(body) + '\n</div>\n')

OUT.parent.mkdir(parents=True, exist_ok=True)
OUT.write_text(page)
print(f'{OUT}  {len(page):,} bytes, {len(body)} blocks')
print('title:', title)
