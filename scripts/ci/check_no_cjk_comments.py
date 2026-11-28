#!/usr/bin/env python3
"""Anti-regression gate: no Chinese (CJK) text in C++/Lua/Python comments
or docstrings.

Scans tracked source files; text inside string literals is NOT flagged —
Chinese user-facing copy (log / UI / error text, fixtures) is allowed by
design; comments and docstrings must be English.

A comment containing `i18n-allow` is exempt (rare, reviewed by hand).
Exit code 1 when any CJK comment text is found.
"""
from __future__ import annotations

import subprocess
import sys

from cjk_comment_lexer import ALLOW_MARK, comment_spans, has_cjk, line_of_offset

EXTS = {"cpp", "cc", "cxx", "h", "hh", "hpp", "hxx", "inl", "ipp", "lua", "py"}


def main() -> int:
    out = subprocess.run(
        ["git", "ls-files", "*.cpp", "*.cc", "*.cxx", "*.h", "*.hh", "*.hpp",
         "*.hxx", "*.inl", "*.ipp", "*.lua", "*.py"],
        capture_output=True, text=True, check=True,
    )
    files = [p for p in out.stdout.splitlines() if p]
    total = 0
    for path in files:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                src = f.read()
        except OSError as e:
            print(f"error: cannot read {path}: {e}", file=sys.stderr)
            total += 1
            continue
        for start, end, kind in comment_spans(path, src):
            text = src[start:end]
            if has_cjk(text) and ALLOW_MARK not in text:
                total += 1
                label = "docstring" if kind == "docstring" else "comment"
                print(f"{path}:{line_of_offset(src, start)}: CJK in {label}: {text.strip()[:120]}")
    if total:
        print(f"\ni18n gate: {total} CJK comment occurrence(s) found — comments must be English.")
        return 1
    print("i18n gate: OK (no CJK in comments)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
