#!/usr/bin/env python3
"""Embed web/designer.html into src/xmas_tree_page.h (run after editing the page)."""
from pathlib import Path
root = Path(__file__).resolve().parent.parent
html = (root / "web" / "designer.html").read_text(encoding="utf-8")
# light minification: drop indentation and blank lines (keeps the page readable in the repo)
lines = [l.strip() for l in html.splitlines()]
html = "\n".join(l for l in lines if l)
assert ')XTPAGE"' not in html
out = ("// Generated from web/designer.html by tools/embed_page.py - do not edit by hand\n"
       "#pragma once\n"
       "static const char XT_PAGE_HTML[] PROGMEM = R\"XTPAGE(" + html + ")XTPAGE\";\n")
(root / "src" / "xmas_tree_page.h").write_text(out, encoding="utf-8")
print(f"wrote src/xmas_tree_page.h ({len(html)} bytes of HTML)")
