#!/usr/bin/env python3
"""Rebuild cpp-postgres-atlas.html from the chapter markdown files.

The published HTML is pre-rendered and self-contained: it carries the 43 mermaid
diagrams as inline SVG and the formulae as KaTeX markup, so it opens with no
network and no JavaScript. Getting there takes two steps, because the renderer is
mermaid itself:

    python build_atlas.py            # 1. rebuild the JS shell from the .md files
    python build_atlas.py --bake     # 2. run it in headless Chrome, keep the DOM

Then print to PDF from the baked file:

    chrome --headless=new --disable-gpu --no-pdf-header-footer \\
           --run-all-compositor-stages-before-draw --virtual-time-budget=60000 \\
           --print-to-pdf=cpp-postgres-atlas.pdf file:///<abs>/cpp-postgres-atlas.html
"""

import argparse
import io
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HTML = os.path.join(HERE, "cpp-postgres-atlas.html")
SHELL = os.path.join(HERE, "cpp-postgres-atlas.shell.html")

CHROME_CANDIDATES = [
    r"C:/Program Files/Google/Chrome/Application/chrome.exe",
    r"C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
]


def find_chrome():
    for path in CHROME_CANDIDATES:
        if os.path.exists(path):
            return path
    sys.exit("No Chrome/Edge found; edit CHROME_CANDIDATES.")


def chapter_files():
    return sorted(f for f in os.listdir(HERE) if re.match(r"^\d\d-.*\.md$", f))


def rebuild_shell():
    """Refresh the DOCS array in the JS shell from the chapter markdown."""
    if not os.path.exists(SHELL):
        sys.exit("Missing %s -- keep the JS shell alongside the baked HTML." % SHELL)

    s = io.open(SHELL, encoding="utf-8").read()
    prefix = "const DOCS = "
    start = s.index(prefix)
    end = s.index(";\n", start)
    docs = json.loads(s[start + len(prefix):end])

    by_path = {d["path"]: d for d in docs}
    rebuilt = []
    for fname in chapter_files():
        entry = by_path.get(fname)
        if entry is None:
            # New chapter: take its title from the leading H1.
            body = io.open(os.path.join(HERE, fname), encoding="utf-8").read()
            m = re.search(r"^#\s+(.+)$", body, re.M)
            entry = {"path": fname, "title": m.group(1) if m else fname}
        entry["md"] = io.open(os.path.join(HERE, fname), encoding="utf-8").read()
        rebuilt.append(entry)

    s = s[:start] + prefix + json.dumps(rebuilt, ensure_ascii=False) + s[end:]
    io.open(SHELL, "w", encoding="utf-8", newline="\n").write(s)
    print("shell rebuilt: %d chapters" % len(rebuilt))


def bake():
    """Render the shell in headless Chrome and keep the resulting DOM."""
    chrome = find_chrome()
    out = subprocess.run(
        [chrome, "--headless=new", "--disable-gpu", "--no-sandbox",
         "--virtual-time-budget=60000", "--dump-dom",
         "file:///" + SHELL.replace("\\", "/")],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    ).stdout

    if not out:
        sys.exit("Chrome produced no DOM.")

    # The module script has already run; its output is now in the markup.
    out, n = re.subn(r'<script type="module">.*?</script>', "", out, flags=re.S)
    if n != 1:
        sys.exit("Expected exactly one module script to strip, found %d." % n)

    # The runtime appends a diagnostic counter to the title; restore the name.
    out = re.sub(r"<title>[^<]*</title>",
                 "<title>PostgreSQL Storage Engine Architecture Atlas</title>", out, count=1)

    svgs = len(re.findall(r'<div class="mermaid"[^>]*><svg', out))
    failed = out.count('<div class="mermaid-failed"')
    chapters = out.count('class="chapter"')
    if failed:
        sys.exit("%d diagrams failed to render." % failed)

    io.open(HTML, "w", encoding="utf-8", newline="\n").write(out)
    print("baked %s: %d chapters, %d inline diagrams, %d script tags"
          % (os.path.basename(HTML), chapters, svgs, out.count("<script")))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--bake", action="store_true",
                    help="render the shell to the self-contained HTML")
    args = ap.parse_args()
    if args.bake:
        bake()
    else:
        rebuild_shell()
