#!/usr/bin/env python3
"""Pre-build source checks for Diablo4AssetBrowser Native.

Catches the classes of mistake that have actually broken this build, cheaply, before a
multi-minute MSVC cycle:

  0. Empty / truncated file         — a botched write that left 0 bytes. Checked FIRST and
                                      alone, because an empty file passes every other check
                                      here: it has balanced delimiters, no bad format strings
                                      and no duplicate lambdas. Two files were blanked and this
                                      script reported "131 file(s) clean".
  1. Unbalanced {} () []            — truncated or mis-spliced edits.
  2. Missing #include for a         — a header-only helper used as `Ns::Thing` with no
     header-only helper               matching include directive. THIS is the one that broke
                                      three translation units: a *comment* mentioning the path
                                      satisfied a naive substring check, so the include was
                                      never added. Only a real directive counts here.
  3. printf-style arg mismatch      — qInfo/qWarning/qDebug/printf format specifiers vs args.
  4. Qt macro collisions            — a local named `emit`/`signals`/`slots` silently vanishes.

Exit code 0 = clean, 1 = problems found. Run from anywhere:

    python verify-src.py                 # check src/
    python verify-src.py --quiet         # only print problems
    python verify-src.py path/to/file    # check specific files
"""

from __future__ import annotations
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SRC = ROOT / "src"

# Header-only helpers: namespace -> header path used in the #include directive.
# Add a row when a new one lands; the check is only as good as this table.
HEADER_ONLY = {
    "ViewportPartMenu": "util/ViewportPartMenu.h",
    # Same header, second namespace: the shared context-menu vocabulary. Guarded separately
    # because a file can use MenuText:: labels without touching ViewportPartMenu::.
    "MenuText":         "util/ViewportPartMenu.h",
    "PanelPersist":     "util/PanelPersist.h",
    "CameraOrbit":      "util/CameraOrbitRow.h",
    "NameTemplate":     "util/NameTemplate.h",
    "ExportLayout":     "util/ExportLayout.h",
    "HoverInfo":        "util/HoverInfo.h",
    "ExportNotifier":   "app/ExportNotifier.h",
}

QT_MACROS = {"emit", "signals", "slots", "foreach"}


def strip_code(text: str) -> str:
    """Blank out strings, char literals and comments with a single-pass scanner.

    A regex pipeline is not good enough here: an earlier version tested for `/*` BEFORE
    stripping `//`, so a `/*` inside a line comment flipped it into block-comment mode and
    swallowed the rest of the file — reporting phantom imbalances on files that compile.
    Handles raw strings R"delim(...)delim" too, which appear in shader sources.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        # line comment
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        # block comment
        if c == "/" and nxt == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")          # keep line numbers usable
                i += 1
            i += 2
            continue
        # raw string R"delim( ... )delim"
        if c == "R" and nxt == '"':
            j = text.find("(", i + 2)
            if j > 0:
                delim = text[i + 2:j]
                close = ')' + delim + '"'
                k = text.find(close, j)
                if k > 0:
                    out.append('""')
                    out.extend("\n" * text.count("\n", i, k))
                    i = k + len(close)
                    continue
        # ordinary string
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            out.append('""')
            continue
        # char literal
        if c == "'":
            i += 1
            while i < n and text[i] != "'":
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            out.append("''")
            continue
        out.append(c)
        i += 1
    return "".join(out)


def check_balance(path: Path, code: str) -> list[str]:
    b = code.count("{") - code.count("}")
    p = code.count("(") - code.count(")")
    s = code.count("[") - code.count("]")
    if (b, p, s) == (0, 0, 0):
        return []
    return [f"unbalanced delimiters: braces {b:+d}, parens {p:+d}, brackets {s:+d}"]


def check_header_only_includes(path: Path, raw: str, code: str) -> list[str]:
    """A namespace used but never included. Matches the DIRECTIVE, not a substring."""
    problems = []
    for ns, header in HEADER_ONLY.items():
        if not re.search(rf"\b{re.escape(ns)}::", code):
            continue                                   # not used here
        # The DEFINING header cannot include itself. Without this, adding a second namespace from
        # an existing header to the table immediately fails that header.
        if path.as_posix().endswith(header):
            continue
        pat = re.compile(rf'^\s*#\s*include\s+["<]{re.escape(header)}[">]\s*$', re.M)
        if not pat.search(raw):
            problems.append(
                f"uses {ns}:: but has no `#include \"{header}\"` directive "
                f"(a comment mentioning the path does NOT count)")
    return problems


FMT_CALL = re.compile(r"\b(qInfo|qWarning|qCritical|qDebug|printf|fprintf)\s*\(", re.M)


CTX_INSTALL_RE = re.compile(r"\b(?:CsvCopy::install|installCopyMenu)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)")
CTX_POLICY_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*->\s*setContextMenuPolicy\s*\(\s*Qt::(\w+)")


def check_ctx_menu_order(text: str) -> list[str]:
    """CsvCopy::install must come AFTER the view sets its own context-menu policy.

    CsvCopy::install only declines to add its Copy/Copy all menu when the view ALREADY has a
    policy set. Called first, it sees DefaultContextMenu, installs a handler, and the caller's
    later connect() adds a SECOND handler to the same signal — Qt runs both, CsvCopy's is
    connected first, so its menu opens and the real one is unreachable until dismissed.

    This shipped in three views (ModelsTab m_list, ModelsTab m_partsView, TexturesTab m_view) and
    is invisible in review: every line is individually correct and the menu simply never changes.
    Cheap to check mechanically, so it is checked on every build.
    """
    problems: list[str] = []
    installs: dict[str, list[int]] = {}
    policies: dict[str, list[tuple[int, str]]] = {}
    for i, line in enumerate(text.split("\n"), 1):
        m = CTX_INSTALL_RE.search(line)
        if m:
            installs.setdefault(m.group(1), []).append(i)
        m2 = CTX_POLICY_RE.search(line)
        if m2:
            policies.setdefault(m2.group(1), []).append((i, m2.group(2)))
    for var, lines_ in installs.items():
        for il in lines_:
            later = [(pl, pk) for pl, pk in policies.get(var, [])
                     if pl > il and pk != "DefaultContextMenu"]
            if later:
                pl, pk = later[0]
                problems.append(
                    f"line {il}: CsvCopy::install({var}) runs BEFORE {var} sets its own "
                    f"context-menu policy at line {pl} (Qt::{pk}) — CsvCopy will install a "
                    f"competing Copy/Copy all menu that hides the real one. Move the install "
                    f"AFTER the setContextMenuPolicy call.")
    return problems


def _split_args(s: str) -> list[str]:
    """Top-level comma split, respecting nesting AND string/char literals.

    The literal handling is the fix for a long-standing false positive: the docstring used to
    claim literals were "already-stripped", but the argument list handed here still contains
    them, so a comma INSIDE a string — qWarning("...", "a, b") or any message containing a
    comma — was counted as an argument separator. Every such call was reported as an arg/spec
    mismatch, and the workaround was to reword messages with em dashes, i.e. the checker was
    quietly dictating prose. Now a literal is skipped whole, escapes included.
    """
    args, depth, cur = [], 0, ""
    i, n = 0, len(s)
    while i < n:
        ch = s[i]
        if ch in "\"'":
            quote = ch
            j = i + 1
            while j < n:
                if s[j] == "\\":       # escape: consume the next char whatever it is
                    j += 2
                    continue
                if s[j] == quote:
                    j += 1
                    break
                j += 1
            cur += s[i:j]
            i = j
            continue
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(cur)
            cur = ""
        else:
            cur += ch
        i += 1
    if cur.strip():
        args.append(cur)
    return args


def _skip_ws(t: str, i: int) -> int:
    while i < len(t):
        if t[i] in " \t\r\n":
            i += 1
        elif t.startswith("//", i):
            i = t.find("\n", i)
            if i < 0:
                return len(t)
        elif t.startswith("/*", i):
            j = t.find("*/", i)
            i = len(t) if j < 0 else j + 2
        else:
            break
    return i


def _read_string_run(t: str, i: int):
    """Consume consecutive "..." literals (C concatenation). Returns (contents, next_index)."""
    parts, saw = [], False
    while True:
        i = _skip_ws(t, i)
        if i >= len(t) or t[i] != '"':
            break
        saw = True
        i += 1
        buf = ""
        while i < len(t) and t[i] != '"':
            if t[i] == "\\" and i + 1 < len(t):
                buf += t[i:i + 2]
                i += 2
                continue
            buf += t[i]
            i += 1
        i += 1
        parts.append(buf)
    return ("".join(parts) if saw else None), i


def check_format_args(path: Path, raw: str) -> list[str]:
    """Compare % specifiers against argument count.

    Parses the call rather than guessing with rindex('"'): arguments frequently CONTAIN string
    literals (ternaries, qPrintable(...)), which made a naive split report every such call as
    having zero arguments. Only the leading concatenated literal run is the format string; if
    the format is not a literal (a variable), the call is skipped rather than guessed at.
    """
    problems = []
    for m in FMT_CALL.finditer(raw):
        fn = m.group(1)
        i = raw.index("(", m.start())
        depth, j = 0, i
        while j < len(raw):
            if raw[j] == "(":
                depth += 1
            elif raw[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if j >= len(raw):
            continue
        body = raw[i + 1:j]
        k = 0
        if fn == "fprintf":                     # first arg is the stream
            args0 = _split_args(body)
            if len(args0) < 2:
                continue
            k = len(args0[0]) + 1
        fmt, k = _read_string_run(body, k)
        if fmt is None:
            continue                            # format is not a literal — cannot check
        specs = [s for s in re.findall(r"%[-+ #0-9.*hlLqjzt]*[diouxXeEfgGaAcspn%]", fmt)
                 if s != "%%"]
        if not specs:
            continue
        rest = body[k:].lstrip()
        if rest.startswith(","):
            rest = rest[1:]
        args = [a for a in _split_args(rest) if a.strip()]
        if len(args) != len(specs):
            line = raw[:m.start()].count("\n") + 1
            problems.append(
                f"line {line}: {fn}() has {len(specs)} format specifier(s) "
                f"but {len(args)} argument(s)")
    return problems


DECL = re.compile(r"\b(?:auto|int|float|double|bool|QString|QMenu|QAction)\s+(\w+)\s*=")

# The DECL list above is a closed set of types and only matches the `TYPE NAME =` form, so it
# missed `QHash<QString, LatestSlot> slots;` — templated type, no initialiser — and misses
# parameters entirely. That cost a full build cycle: `slots` expands to nothing, so the line became
# `QHash<QString, LatestSlot> ;` and every later `slots.insert(...)` compiled as `.insert(...)`.
# MSVC then reports "syntax error: '.'" pointing at correct-looking code, several functions away
# from the actual mistake, which is close to the worst possible diagnostic.
#
# Two broader patterns, both keyed on the macro name being USED as an identifier:
#   USE  — member access. `slots.` / `signals->` / `slots[` cannot be anything but a mistake.
#   DECL2 — `<type> NAME` followed by ; = , ) — covers locals, members and parameters.
# Neither fires on the legitimate spellings: `public slots:` and `signals:` are followed by ':',
# and `emit obj.sig()` captures `obj`, not `emit`.
QT_MACRO_USE = re.compile(r"\b(emit|signals|slots|foreach)\s*(?:\.|->|\[)")
QT_MACRO_DECL2 = re.compile(r"[>\w\]]\s*[&*]?\s+(emit|signals|slots|foreach)\s*[;=,)]")


def check_qt_macro_names(path: Path, code: str) -> list[str]:
    problems = []
    seen = set()

    def add(pos: int, name: str, why: str) -> None:
        line = code[:pos].count("\n") + 1
        if (line, name) in seen:
            return
        seen.add((line, name))
        problems.append(
            f"line {line}: `{name}` is a Qt macro that expands to NOTHING — {why}. "
            f"Rename the identifier.")

    for m in DECL.finditer(code):
        if m.group(1) in QT_MACROS:
            add(m.start(), m.group(1), "the declaration silently disappears")
    for m in QT_MACRO_USE.finditer(code):
        add(m.start(), m.group(1), "used here as an object, so this line loses its subject")
    for m in QT_MACRO_DECL2.finditer(code):
        add(m.start(), m.group(1), "declared here as a variable or parameter")
    return problems


BODY = re.compile(r"^(?:\w[\w:<>,~\s\*&]*?)\b(\w+::\w+)\s*\([^;{]*\)\s*(?:const\s*)?\{", re.M)
LOCAL = re.compile(r"^\s{4,}auto\s+(\w+)\s*=\s*\[", re.M)


def check_duplicate_locals(path: Path, code: str) -> list[str]:
    """Two `auto NAME = [...]` at the same brace depth inside one function body.

    Splicing a lambda body into a new member function easily duplicates the helper lambdas it
    already declared — MSVC reports 'redefinition; multiple initialization' for each, three
    errors per name, and it costs a whole build cycle to find out. Cheap to catch here.
    """
    problems = []
    for m in BODY.finditer(code):
        start = m.end() - 1
        depth, i, n = 0, start, len(code)
        while i < n:
            if code[i] == "{":
                depth += 1
            elif code[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        body = code[start:i]
        seen = {}
        for d in LOCAL.finditer(body):
            name = d.group(1)
            if name in seen:
                line = code[:start + d.start()].count("\n") + 1
                problems.append(
                    f"line {line}: `{name}` declared twice in {m.group(1)}() — "
                    f"duplicate lambda (MSVC: 'redefinition; multiple initialization')")
            else:
                seen[name] = True
    return problems


def check_truncation(path: Path, raw: str) -> list[str]:
    """Empty or near-empty source file — almost always a botched write, not intent.

    THIS EXISTS BECAUSE THE OTHER CHECKS CANNOT SEE IT. An editing script that opened a file
    for writing before reading it truncated main.cpp and CacheVersioning.h to ZERO BYTES, and
    this script reported "131 file(s) clean" — an empty file has balanced delimiters, no bad
    format strings and no duplicate lambdas. It passed every test with flying colours because
    there was nothing left to test.

    A .cpp/.h in this tree is never legitimately empty: even the thinnest header carries
    `#pragma once` and a comment. The floor is deliberately low (a handful of bytes) so this
    only ever fires on real damage, never on a small-but-real file.
    """
    stripped = raw.strip()
    if not stripped:
        return ["FILE IS EMPTY (0 bytes of content) — almost certainly a truncated write. "
                "Restore it from .Backups/ before doing anything else."]
    # A file with no directive, no comment and no brace is not plausibly source.
    if len(stripped) < 24 and not any(t in stripped for t in ("#", "//", "{", ";")):
        return [f"file is only {len(stripped)} byte(s) and contains no code — "
                f"looks truncated; check .Backups/ before building"]
    return []


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    quiet = "--quiet" in sys.argv
    if args:
        files = [Path(a) for a in args]
    else:
        files = sorted(list(SRC.rglob("*.cpp")) + list(SRC.rglob("*.h")))
    if not files:
        print(f"verify-src: no sources found under {SRC}")
        return 1

    total = 0
    for f in files:
        try:
            raw = f.read_text(encoding="utf-8", errors="replace")
        except OSError as e:
            print(f"[FAIL] {f}: {e}")
            total += 1
            continue
        code = strip_code(raw)
        # Truncation FIRST: on an empty file every other check trivially passes, so reporting
        # "clean" is worse than useless — it actively certifies the damage.
        trunc = check_truncation(f, raw)
        if trunc:
            total += len(trunc)
            rel = f.relative_to(ROOT) if ROOT in f.parents or f.is_relative_to(ROOT) else f
            print(f"\n[FAIL] {rel}")
            for p in trunc:
                print(f"       - {p}")
            continue
        problems = (check_balance(f, code)
                    + check_header_only_includes(f, raw, code)
                    + check_format_args(f, raw)
                    + check_qt_macro_names(f, code)
                    + check_duplicate_locals(f, code)
                    + check_ctx_menu_order(raw))
        if problems:
            total += len(problems)
            rel = f.relative_to(ROOT) if ROOT in f.parents or f.is_relative_to(ROOT) else f
            print(f"\n[FAIL] {rel}")
            for p in problems:
                print(f"       - {p}")

    if total == 0:
        if not quiet:
            print(f"verify-src: OK — {len(files)} file(s) clean "
                  f"(non-empty, balance, header-only includes, format args, Qt macro names, "
                  f"duplicate lambdas)")
        return 0
    print(f"\nverify-src: {total} problem(s) in {len(files)} file(s) — fix before building.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
