"""Sanity-check the two READMEs after translation.

Looks for the things a rewrite tends to break: unbalanced code fences, table
rows that no longer line up, stray English paragraphs left behind, and CR
characters introduced by a Windows editor.
"""
import re
import sys

FENCE = chr(96) * 3

for name in ["README.md", "README.en.md"]:
    text = open(name, encoding="utf-8").read()
    lines = text.split("\n")

    fences = sum(1 for l in lines if l.strip().startswith(FENCE))
    table = [l for l in lines if l.strip().startswith("|")]
    heads = [l for l in lines if l.startswith("#")]
    non_ascii = sum(1 for c in text if ord(c) > 127)

    # Table rows must have a consistent column count.
    widths = {}
    for l in table:
        n = l.count("|")
        widths.setdefault(n, 0)
        widths[n] += 1

    # Paragraph lines that still look like untranslated English prose.
    english = []
    for l in lines:
        s = l.strip()
        if not s or s[0] in "|>#-*`":
            continue
        # Two or more consecutive lowercase words after a capitalised start.
        if re.match(r"^[A-Z][a-z]+ [a-z]+ [a-z]+", s):
            english.append(s)

    print(f"{name}")
    print(f"  code fences      : {fences}  "
          f"{'balanced' if fences % 2 == 0 else 'UNBALANCED'}")
    print(f"  table rows       : {len(table)}  column counts: {widths}")
    print(f"  headings         : {len(heads)}")
    print(f"  non-ascii chars  : {non_ascii}")
    print(f"  CR characters    : {'none' if chr(13) not in text else 'PRESENT'}")
    print(f"  untranslated EN  : {len(english)}")
    for e in english[:3]:
        print(f"      | {e[:70]}")
    print()
