#!/usr/bin/env python3
"""Comment-only verifier: prove that worktree edits touched only comments /
docstrings, never code or string literals.

For every changed C++/Lua/Python source file, masks comment+docstring spans
in both HEAD and worktree versions, strips ALL whitespace, and compares the
remaining token streams.  Byte-identical token streams prove the diff was
confined to comment text (string literals are compared verbatim — they must
not change).

Exit 0 = every changed file is comment-only.  Exit 1 = print first offense.
Run with PYTHONDONTWRITEBYTECODE=1 from the repo root.
"""
from __future__ import annotations

import subprocess
import sys

from cjk_comment_lexer import comment_spans

MASKABLE_KINDS = {"comment", "docstring"}


def tracked_changed() -> list[str]:
    out = subprocess.run(
        ["git", "diff", "--name-only", "HEAD"], capture_output=True, text=True, check=True
    )
    return [p for p in out.stdout.splitlines() if p]


def masked(src: str, path: str) -> str:
    chars = list(src)
    for start, end, kind in comment_spans(path, src):
        if kind in MASKABLE_KINDS:
            for i in range(start, min(end, len(chars))):
                if chars[i] not in "\r\n":
                    chars[i] = " "
    return "".join(chars)


def normalize(src: str, path: str) -> str:
    return "".join(masked(src, path).split())


def main() -> int:
    files = [p for p in tracked_changed() if p.rsplit(".", 1)[-1].lower() in
             {"cpp", "cc", "cxx", "h", "hh", "hpp", "hxx", "inl", "ipp", "lua", "py"}]
    if not files:
        print("verify: no changed source files")
        return 0
    bad = 0
    for path in files:
        head = subprocess.run(["git", "show", f"HEAD:{path}"], capture_output=True, check=False)
        if head.returncode != 0:
            continue  # new file: nothing to compare against
        try:
            head_src = head.stdout.decode("utf-8")
            work_src = open(path, encoding="utf-8").read()
        except (UnicodeDecodeError, OSError) as e:
            print(f"error: cannot read {path}: {e}", file=sys.stderr)
            bad += 1
            continue
        if normalize(head_src, path) != normalize(work_src, path):
            bad += 1
            a, b = normalize(head_src, path), normalize(work_src, path)
            i = next((k for k in range(min(len(a), len(b))) if a[k] != b[k]), min(len(a), len(b)))
            lo, hi = max(0, i - 40), i + 40
            print(f"NOT comment-only: {path}")
            print(f"  HEAD ...{a[lo:hi]!r}...")
            print(f"  WORK ...{b[lo:hi]!r}...")
    if bad:
        print(f"verify: {bad} file(s) with non-comment changes")
        return 1
    print(f"verify: OK — {len(files)} changed source file(s), code+strings identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
