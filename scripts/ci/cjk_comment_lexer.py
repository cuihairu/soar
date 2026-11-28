#!/usr/bin/env python3
"""Lexical comment/docstring span extraction for apollo (C++ / Lua / Python).

One API for the i18n campaign tooling:

    spans = comment_spans(path, src)   # [(start, end, kind), ...]
    has_cjk(text)                      # True when text contains CJK ideographs

kinds: "comment" (// /* */ -- #[[ ]]) or "docstring" (Python docstrings only).

C++: line/ block comments, string and char literals with escapes, raw strings
R"delim(...)delim".  Lua: -- line, --[[...]] / --[=[...]=] blocks, short and
long strings.  Python: tokenize-based; COMMENT tokens plus docstrings (the
first string statement of a module / class / function).

Strings are NOT spans: Chinese string literals (log / UI / error copy) must
stay byte-identical by campaign rule.
"""
from __future__ import annotations

import io
import re
import sys
import tokenize

# CJK unified ideographs + ext A + compat ideographs
_CJK_RE = re.compile("[㐀-䶿一-鿿豈-﫿]")
ALLOW_MARK = "i18n-allow"


def has_cjk(text: str) -> bool:
    return bool(_CJK_RE.search(text))


def line_of_offset(src: str, off: int) -> int:
    return src.count("\n", 0, off) + 1


def _cpp_spans(src: str) -> list[tuple[int, int, str]]:
    spans: list[tuple[int, int, str]] = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n:
            if src[i + 1] == "/":
                j = i + 2
                while j < n and src[j] != "\n":
                    if src[j] == "\\" and j + 1 < n and src[j + 1] == "\n":
                        j += 2  # line continuation: // comment spills to next line
                        continue
                    j += 1
                spans.append((i, j, "comment"))
                i = j
                continue
            if src[i + 1] == "*":
                j = src.find("*/", i + 2)
                j = n if j < 0 else j + 2
                spans.append((i, j, "comment"))
                i = j
                continue
        if c == '"':
            m = re.match(r'R"([^()\\ \t\n]*)\(', src[i:])
            if m:  # raw string R"delim( ... )delim"
                closer = ')' + m.group(1) + '"'
                j = src.find(closer, i + m.end())
                j = n if j < 0 else j + len(closer)
                i = j
                continue
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == '"' or src[j] == "\n":
                    break
                j += 1
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == "'" or src[j] == "\n":
                    break
                j += 1
            i = j + 1
            continue
        i += 1
    return spans


def _lua_long_level(src: str, i: int) -> int | None:
    """If src[i:] starts a long-bracket open [[ or [=[, return its level."""
    m = re.match(r"\[(=*)\[", src[i:])
    return len(m.group(1)) if m else None


def _lua_spans(src: str) -> list[tuple[int, int, str]]:
    spans: list[tuple[int, int, str]] = []
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c == "-" and src[i + 1 : i + 2] == "-":
            lvl = _lua_long_level(src, i + 2)
            if lvl is not None:  # block comment --[[ ... ]] / --[=[ ... ]=]
                closer = "]" + "=" * lvl + "]"
                j = src.find(closer, i + 2)
                j = n if j < 0 else j + len(closer)
                spans.append((i, j, "comment"))
                i = j
                continue
            j = i + 2
            while j < n and src[j] != "\n":
                j += 1
            spans.append((i, j, "comment"))
            i = j
            continue
        if c == '"' or c == "'":
            j = i + 1
            while j < n:
                if src[j] == "\\":
                    j += 2
                    continue
                if src[j] == c or src[j] == "\n":
                    break
                j += 1
            i = j + 1
            continue
        lvl = _lua_long_level(src, i)
        if lvl is not None:  # long string [[ ... ]] (no preceding --)
            closer = "]" + "=" * lvl + "]"
            j = src.find(closer, i + 1)
            j = n if j < 0 else j + len(closer)
            i = j
            continue
        i += 1
    return spans


def _py_spans(src: str) -> list[tuple[int, int, str]]:
    spans: list[tuple[int, int, str]] = []
    lines = src.splitlines(keepends=True)
    line_starts = [0]
    for line in lines:
        line_starts.append(line_starts[-1] + len(line))

    # Python 3.14 AST col_offset / end_col_offset are UTF-8 BYTE offsets
    # within the line; older versions use character offsets.  tokenize's
    # positions are always character offsets.  Convert both to absolute
    # character offsets so comment/docstring spans are exact on any version.
    def off(pos: tuple[int, int], *, byte_col: bool = False) -> int:
        line, col = pos
        start = line_starts[line - 1]
        if not byte_col:
            return start + col
        raw = lines[line - 1].encode("utf-8")
        return start + len(raw[:col].decode("utf-8", errors="ignore"))

    try:
        for tok in tokenize.generate_tokens(io.StringIO(src).readline):
            if tok.type == tokenize.COMMENT:
                spans.append((off(tok.start), off(tok.end), "comment"))
    except tokenize.TokenError:
        pass
    # Docstrings: authoritative via AST (first string statement of module /
    # class / function). Treated as translatable documentation, not strings.
    try:
        import ast

        tree = ast.parse(src)
        byte_cols = sys.version_info >= (3, 14)
        for node in ast.walk(tree):
            if isinstance(node, (ast.Module, ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
                body = node.body
                if (
                    body
                    and isinstance(body[0], ast.Expr)
                    and isinstance(body[0].value, ast.Constant)
                    and isinstance(body[0].value.value, str)
                ):
                    a = body[0]
                    spans.append(
                        (
                            off((a.lineno, a.col_offset), byte_col=byte_cols),
                            off((a.end_lineno, a.end_col_offset), byte_col=byte_cols),
                            "docstring",
                        )
                    )
    except SyntaxError:
        pass
    spans.sort()
    return spans


_LEXERS = {
    ".cpp": _cpp_spans,
    ".cc": _cpp_spans,
    ".cxx": _cpp_spans,
    ".h": _cpp_spans,
    ".hh": _cpp_spans,
    ".hpp": _cpp_spans,
    ".hxx": _cpp_spans,
    ".inl": _cpp_spans,
    ".ipp": _cpp_spans,
    ".lua": _lua_spans,
    ".py": _py_spans,
}


def comment_spans(path: str, src: str) -> list[tuple[int, int, str]]:
    """[(start, end, kind)] for the given path's language, sorted by start."""
    dot = path.rsplit(".", 1)[-1].lower()
    lexer = _LEXERS.get("." + dot)
    if lexer is None:
        return []
    return lexer(src)


def cjk_comment_spans(path: str, src: str) -> list[tuple[int, int, str]]:
    return [s for s in comment_spans(path, src) if has_cjk(src[s[0] : s[1]])]
