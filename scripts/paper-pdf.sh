#!/usr/bin/env bash
#
# paper-pdf.sh — the paper as a PDF, from committed sources.
#
#   scripts/paper-pdf.sh [out.pdf]     # default: build/paper/as-on-itch.pdf
#
# The chain is: as-on-itch.md -> as-on-itch.html -> PDF. The first hop is
# scripts/paper-html.py, stdlib-only and checked by CI, so the HTML can never
# drift from the Markdown. The last hop needs a print engine, and that is the
# one part of this repository that depends on what happens to be installed:
# Chrome/Chromium headless, or wkhtmltopdf, or pandoc with a LaTeX engine.
#
# The PDF is therefore a BUILD OUTPUT and is not committed. CI checks the
# Markdown, the generated blocks inside it, the figures, and the HTML — every
# input to the PDF — and does not check the PDF itself, because a binary whose
# bytes depend on a font cache is not something a diff can referee.
#
# This script deliberately does NOT fall back to "print the Markdown as plain
# text and call it a paper". If there is no engine it says so and exits 1.
set -uo pipefail
cd "$(dirname "$0")/.."

out=${1:-build/paper/as-on-itch.pdf}
html=docs/paper/as-on-itch.html

python3 scripts/paper-html.py || exit 1
mkdir -p "$(dirname "$out")"
abs_html="file://$(pwd)/$html"

# $PAPER_PDF_ENGINE wins, for a Chrome that is installed somewhere this list
# would never think to look.
for c in ${PAPER_PDF_ENGINE:+"$PAPER_PDF_ENGINE"} \
         chromium chromium-browser google-chrome google-chrome-stable \
         "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"; do
    if command -v "$c" >/dev/null 2>&1 || [[ -x $c ]]; then
        "$c" --headless --disable-gpu --no-sandbox \
             --print-to-pdf="$out" --no-pdf-header-footer "$abs_html" \
             >/dev/null 2>&1 && [[ -s $out ]] && {
            echo "wrote $out  ($(wc -c <"$out" | tr -d ' ') bytes, via $c)"; exit 0; }
    fi
done

if command -v wkhtmltopdf >/dev/null 2>&1; then
    wkhtmltopdf --enable-local-file-access "$html" "$out" >/dev/null 2>&1 &&
        [[ -s $out ]] && { echo "wrote $out (via wkhtmltopdf)"; exit 0; }
fi

if command -v pandoc >/dev/null 2>&1; then
    # Straight from the Markdown; pandoc does its own layout, so this route
    # ignores the HTML's stylesheet rather than pretending to honour it.
    pandoc docs/paper/as-on-itch.md -o "$out" --pdf-engine=xelatex \
        -V geometry:margin=2cm >/dev/null 2>&1 &&
        [[ -s $out ]] && { echo "wrote $out (via pandoc; its own layout, not the CSS)"; exit 0; }
fi

cat >&2 <<'MSG'
No print engine found. The HTML is built and current at
docs/paper/as-on-itch.html — open it and print to PDF, or install one of:

  chromium / google-chrome   (headless --print-to-pdf; honours the print CSS)
  wkhtmltopdf                (honours most of it)
  pandoc + xelatex           (its own layout)

Or point PAPER_PDF_ENGINE at a Chrome binary this list does not know about:

  PAPER_PDF_ENGINE=/path/to/chrome scripts/paper-pdf.sh
MSG
exit 1
