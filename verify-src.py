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
  5. Duplicate QHash/QMap keys      — a repeated key in one brace initializer. The LAST literal
                                      wins, so a later guess silently overwrote two measured
                                      entries in SnoIndex::groupNameMap and the correction
                                      shipped doing nothing. Compiles, links, reads fine.

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


MAP_CONTAINERS = ("QHash", "QMap", "std::map", "std::unordered_map")


def strip_comments(text: str) -> str:
    """Comments out, string CONTENTS kept — strip_code blanks strings, which this check needs."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and nxt == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
            continue
        if c in "\"'":
            q, out_start = c, i
            i += 1
            while i < n and text[i] != q:
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            out.append(text[out_start:i])
            continue
        out.append(c)
        i += 1
    return "".join(out)


def check_duplicate_map_keys(path: Path, raw: str) -> list[str]:
    """A key repeated inside one QHash/QMap brace initializer.

    THIS EXISTS BECAUSE IT SILENTLY UNDID A CORRECTION. SnoIndex::groupNameMap() set
    {140,"Face"} and {152,"AppearanceSet"} from measured CoreTOC data, and twelve lines later
    the same initializer repeated 140 and 152 as "BattlePassTier" and "TrackedReward". A brace
    initializer keeps the LAST literal for a repeated key, so two guesses overwrote two facts,
    the fix appeared in the diff, shipped, and did nothing. Nothing warned: it compiles, it is
    not a duplicate symbol, and the file reads correctly unless you happen to compare every key
    against every other one.

    Scoped to the associative containers ONLY. An array of structs may legitimately repeat its
    first member, so flagging every `{a, b}` list would be noise; in a QHash/QMap initializer a
    repeated key is a bug every time.
    """
    problems = []
    text = strip_comments(raw)
    for m in re.finditer(r"=\s*\{", text):
        head = text[max(0, m.start() - 220):m.start()]
        head = head[head.rfind(";") + 1:]                 # this declaration only
        if not any(c in head for c in MAP_CONTAINERS):
            continue
        # Walk to the matching close brace, collecting the elements one level in.
        i, depth, n = m.end() - 1, 0, len(text)
        elems = []                                        # (key, offset)
        while i < n:
            ch = text[i]
            if ch in "\"'":                                # skip a literal wholesale
                q = ch
                i += 1
                while i < n and text[i] != q:
                    i += 2 if text[i] == "\\" else 1
                i += 1
                continue
            if ch == "{":
                depth += 1
                if depth == 2:                            # start of one element
                    j, d2 = i + 1, 0
                    while j < n:
                        c2 = text[j]
                        if c2 in "\"'":
                            q = c2
                            j += 1
                            while j < n and text[j] != q:
                                j += 2 if text[j] == "\\" else 1
                        elif c2 in "{([":
                            d2 += 1
                        elif c2 in "})]":
                            if d2 == 0:
                                break
                            d2 -= 1
                        elif c2 == "," and d2 == 0:
                            break
                        j += 1
                    elems.append((" ".join(text[i + 1:j].split()), i))
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if len(elems) < 3:                                # not a lookup table
            continue
        seen = {}
        for key, off in elems:
            if not key:
                continue
            if key in seen:
                line = text[:off].count("\n") + 1
                problems.append(
                    f"line {line}: key {key} repeated in this QHash/QMap initializer "
                    f"(first at line {seen[key]}) — the LAST literal wins, so the earlier "
                    f"entry is silently discarded")
            else:
                seen[key] = text[:off].count("\n") + 1
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


# ── Direct d4data JSON reads ────────────────────────────────────────────────────────────────────
# A whole-tree check, not a per-file one, so it lives outside the per-file loop.
#
# WHY. Opening <d4>/json/base/meta/Material|Appearance/<name>.json directly is the single most
# repeated defect shape in this project, and it always fails the same silent way: an ENCRYPTED
# record ships no JSON, the read returns nothing, and the caller substitutes a default instead of
# reporting a gap. Three separate user-visible bugs traced to exactly this in one session —
# the DOOM StoreProducts discarded from the Catalogue, the Wardrobe weapon roster coming back
# empty (white weapons), and the emissive colour defaulting to white (blown-out glows). Every one
# was found by someone noticing something missing, never by the tool.
#
# MaterialDecode already states the rule in its own header: "THE ONLY correct way to read a
# material's textures ... Use this, never QFile." texturesFor / appearanceRosterAny /
# appearanceRosterFromMeta fall back to the CASC meta binary; a raw QFile cannot.
#
# WARN, DO NOT BLOCK. There are 39 of these outside the sanctioned readers today. Failing on all
# of them would just teach everyone to pass --quiet. So the existing debt is recorded per file as
# a baseline and only an INCREASE fails — new debt is caught the day it is written, old debt is
# paid down deliberately.
D4_JSON_RE = re.compile(r"json/base/meta/(Material|Appearance)/")

# Files that read this JSON legitimately and must not be counted as debt:
#   MaterialDecode.cpp — readMat() IS the sanctioned reader, and appearanceRoster() is the JSON
#                        half of appearanceRosterAny()'s two routes.
#   MatSnoSweep.cpp    — the audits and dumps read the JSON deliberately, as GROUND TRUTH to
#                        score a binary derivation against. That is the opposite of the defect.
D4_JSON_ALLOWED = {
    "src/model/MaterialDecode.cpp",
    "src/index/MatSnoSweep.cpp",
}


def _strip_exists_probes(body: str) -> str:
    """Drop every QFile::exists(...) argument span.

    The rule exists because a raw READ of a missing record returns nothing and the caller
    substitutes a default, silently. A presence PROBE is the opposite: its only output is
    "present" or "absent", which is the very fact the rule wants surfaced — the diagnostics use it
    to report that an appearance ships no .app.json, or that a roster names a material with no
    .mat.json on disk. Counting those as debt would push the dumps toward asserting presence
    instead of measuring it.

    Parenthesis-balanced rather than regex, because these paths are built with QStringLiteral(...)
    .arg(...) chains that contain their own brackets and routinely wrap across three lines.

    CAVEAT, and it is a real one: the exemption only fires when the path is built INLINE. Hoisting
    it into a local — const QString p = QStringLiteral("…/Material/x.mat.json"); QFile::exists(p) —
    re-arms the count, so this check quietly rewards the denser spelling. Anything wrapped in an
    exists() call is also erased wholesale, so exists(helperThatAlsoReads(path)) would slip
    through. Both are acceptable for a heuristic whose job is to catch NEW debt the day it lands;
    neither should be mistaken for a guarantee.
    """
    out, i = [], 0
    needle = "QFile::exists("
    while True:
        j = body.find(needle, i)
        if j < 0:
            out.append(body[i:])
            return "".join(out)
        out.append(body[i:j])
        k = j + len(needle)
        depth = 1
        while k < len(body) and depth:
            if body[k] == "(":
                depth += 1
            elif body[k] == ")":
                depth -= 1
            k += 1
        if depth:
            # Unbalanced. Reachable without any malformed C++: the caller strips comments with
            # line.split("//")[0], so a "//" inside a string literal — QFile::exists(url) on an
            # "http://…" path — truncates the line mid-literal and eats the closing paren. Skip
            # only THIS occurrence rather than abandoning the rest of the file, so one odd literal
            # cannot re-arm every later probe in it. Failing this way can only over-count (a
            # spurious FAIL that names the file), never hide a read.
            out.append(needle)
            i = j + len(needle)
            continue
        i = k

# Measured 2026-09-08. Lower a number when the site is converted; never raise one.
D4_JSON_BASELINE = {
    "src/tabs/ModelsTab.cpp":        14,
    "src/tabs/WardrobeTab2.cpp":      9,   # 12 -> 9: fxScalar, emissiveColorOf, shaderMapOf
    "src/tabs/StableTab2.cpp":        4,   # 7 -> 4: Appearance .app.json paths now via apprJsonPath()
    "src/tabs/ModelsTab_Export.cpp":  5,
    "src/model/ModelParser.cpp":      1,
}


def check_d4_json_reads(files: list) -> tuple:
    """Returns (failures, warnings). A file over its baseline fails; the rest is reported."""
    counts = {}
    for f in files:
        try:
            raw = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        # Line comments dropped so a doc comment naming the path is not counted as a read.
        body = "\n".join(l.split("//")[0] for l in raw.splitlines())
        n = len(D4_JSON_RE.findall(_strip_exists_probes(body)))
        if not n:
            continue
        rel = f.as_posix()
        for root in ("src/",):
            i = rel.find(root)
            if i >= 0:
                rel = rel[i:]
                break
        if rel in D4_JSON_ALLOWED:
            continue
        counts[rel] = n
    fails, warns = [], []
    for rel, n in sorted(counts.items()):
        base = D4_JSON_BASELINE.get(rel, 0)
        if n > base:
            fails.append(f"{rel}: {n} direct d4data JSON read(s), baseline {base} — "
                         f"route new ones through MaterialDecode (texturesFor / "
                         f"appearanceRosterAny), which falls back to the CASC meta binary")
        elif n < base:
            warns.append(f"{rel}: {n} left (baseline {base}) — lower the baseline in verify-src.py")
        else:
            warns.append(f"{rel}: {n}")
    return fails, warns


# ── Settings-key hygiene ────────────────────────────────────────────────────────────────────────
# Three checks over one idea: QSettings is this project's largest silent-failure surface. A key is
# just a string, nothing validates it, and every way of getting it slightly wrong produces a control
# that looks like it works. The three shapes below are the ones that have actually shipped here:
#
#   1. A key written and never read     — the control does nothing, forever. Four were found by hand
#                                         in the Stable parity audit; two more are still open (R5).
#   2. A combo persisted by currentText() — the DISPLAY string is not an identity. Relabel the item
#                                         and every saved profile silently loses that selection.
#   3. A character-vs-equipment test on a material NAME — "head" is a substring of wolfHead, "brow"
#                                         of browplate, "lash" of backlash. The authored slot tag
#                                         answers this; a substring cannot.
#
# All three are INVENTORY + BASELINE, like the d4data check above: existing entries are listed, and
# only growth fails. None of them can prove a defect on its own — each says "a human should look at
# this one", which is precisely what did not happen for any of the bugs above.


def _settings_bodies(files: list) -> dict:
    """path -> source with line comments stripped. Shared by the three checks below."""
    out = {}
    for f in files:
        try:
            raw = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        rel = f.as_posix()
        i = rel.find("src/")
        out[rel[i:] if i >= 0 else rel] = "\n".join(l.split("//")[0] for l in raw.splitlines())
    return out


# ── 1. Keys written and never read ─────────────────────────────────────────────────────────────
# Accepted entries, with the reason each is not a defect. Empty is the goal; add to it only with a
# reason a reader can check, never to silence the tool.
SETTINGS_DEAD_ALLOWED = {
    # (none)
}

_SET_LITERAL = re.compile(r'setValue\s*\(\s*QStringLiteral\(\s*"([A-Za-z0-9_]+/[^"%]*)"')
_REMOVE_LITERAL = r'remove\s*\(\s*QStringLiteral\(\s*"%s"'
_SETVALUE_LITERAL = r'setValue\s*\(\s*QStringLiteral\(\s*"%s"'
# QStringLiteral("wardrobe2/viewport/") + key      → the whole family is reachable by a helper
_CONCAT_PREFIX = re.compile(r'QStringLiteral\(\s*"([A-Za-z0-9_/]*/)"\s*\)\s*\+')
# prefix + QStringLiteral("/_loading")             → the leaf is the literal, the group is computed
_CONCAT_TAIL = re.compile(r'\+\s*QStringLiteral\(\s*"(/[A-Za-z0-9_/]+)"')


def check_dead_settings_keys(files: list) -> tuple:
    """A key written with a literal whose literal is read nowhere — directly or via a helper."""
    bodies = _settings_bodies(files)
    whole = "\n".join(bodies.values())
    written = {}
    for rel, body in bodies.items():
        for k in _SET_LITERAL.findall(body):
            written.setdefault(k, set()).add(rel)
    # Concatenation is how most of this codebase reads grouped keys, and ignoring it made the first
    # draft of this check report fifteen keys of which fourteen were fine. Both spellings count.
    prefixes = set(_CONCAT_PREFIX.findall(whole))
    tails = set(_CONCAT_TAIL.findall(whole))

    def is_read(k: str) -> bool:
        occurrences = whole.count('"%s"' % k)
        occurrences -= len(re.findall(_SETVALUE_LITERAL % re.escape(k), whole))
        occurrences -= len(re.findall(_REMOVE_LITERAL % re.escape(k), whole))
        if occurrences > 0:
            return True                       # named literally somewhere that is not a write
        for p in prefixes:                    # value(QStringLiteral("group/") + leaf)
            if k.startswith(p) and "/" not in k[len(p):]:
                return True
        for t in tails:                       # value(group + QStringLiteral("/leaf"))
            if k.endswith(t):
                return True
        return False

    fails, warns = [], []
    for k in sorted(written):
        if is_read(k) or k in SETTINGS_DEAD_ALLOWED:
            continue
        where = ", ".join(sorted(written[k]))
        fails.append(f'"{k}" is written by {where} and read nowhere — either wire up the read, '
                     f"delete the write, or record it in SETTINGS_DEAD_ALLOWED with a reason")
    return fails, warns


# ── 2. Combos persisted by their display text ──────────────────────────────────────────────────
# Reviewed 2026-09-12. Every site below stores the LABEL on purpose, because the label is the
# identity at that layer — the armour slots, the look presets and the theme resolver all speak
# appearance NAMES end to end, and findText is how they are restored. skinTone/skinDetail were the
# ones that did not fit that pattern (their items carry a colour and a style token their consumers
# actually use) and have been converted to currentData(). A count ABOVE the baseline means a new
# combo is being persisted by label — check whether its items carry userData first.
SETTINGS_TEXT_BASELINE = {
    "src/tabs/WardrobeTab2.cpp": 4,   # slot/%1 x2 (gender-swap + the handler), weaponType, weaponType2
}
_TEXT_PERSIST = re.compile(r'setValue\s*\([^;]{0,240}?currentText\s*\(\s*\)', re.S)


def check_text_persisted_combos(files: list) -> tuple:
    bodies = _settings_bodies(files)
    fails, warns = [], []
    for rel, body in sorted(bodies.items()):
        n = len(_TEXT_PERSIST.findall(body))
        if not n:
            continue
        base = SETTINGS_TEXT_BASELINE.get(rel, 0)
        if n > base:
            fails.append(f"{rel}: {n} combo(s) persisted by currentText(), baseline {base} — "
                         f"store currentData() unless the LABEL really is the identity at that "
                         f"layer, then raise the baseline with the reason")
        elif n < base:
            warns.append(f"{rel}: {n} left (baseline {base}) — lower the baseline in verify-src.py")
        else:
            warns.append(f"{rel}: {n}")
    return fails, warns


# ── 3. Character-vs-equipment decided by a material NAME ───────────────────────────────────────
# Reviewed 2026-09-12, after palM_stor164_wolfHead — a PAULDRON ornament — matched contains("head")
# and took the whole Paladin torso off screen with the HED toggle. The fix was not a better name
# test; it was to gate on primSlot, which is authored data that answers the question directly.
# These counts are the remaining name tests. They are not all wrong — some decide SHADING, where no
# slot tag applies — but every one of them is a place where a material name is being asked a
# question it cannot answer, so a NEW one has to be argued for.
CHAR_NAME_TOKENS = ("head", "face", "body", "skin", "hair", "eyeball",
                    "brow", "lash", "tooth", "teeth", "tongue", "mouth", "_hed", "_bod")
CHAR_NAME_BASELINE = {
    "src/tabs/ModelsTab.cpp":        2,   # hair, skin — shading only, no slot tag exists there
    "src/tabs/ModelsTab_Export.cpp": 1,   # _HED, export scope
    "src/tabs/StableTab2.cpp":       1,   # hair — mounts have no character/equipment split
    "src/tabs/WardrobeTab2.cpp":    18,   # the classification loop; isHead/headCore/hed now slot-gated
}
_NAME_TEST = re.compile(r'contains\s*\(\s*QLatin1String\(\s*"([^"]+)"')


def check_character_name_tests(files: list) -> tuple:
    bodies = _settings_bodies(files)
    fails, warns = [], []
    for rel, body in sorted(bodies.items()):
        n = sum(1 for t in _NAME_TEST.findall(body) if t.lower() in CHAR_NAME_TOKENS)
        if not n:
            continue
        base = CHAR_NAME_BASELINE.get(rel, 0)
        if n > base:
            fails.append(f"{rel}: {n} character-token name test(s), baseline {base} — a material "
                         f"NAME cannot tell the character from a worn item (wolfHead, browplate, "
                         f"backlash). Gate on the slot tag, or raise the baseline with the reason")
        elif n < base:
            warns.append(f"{rel}: {n} left (baseline {base}) — lower the baseline in verify-src.py")
        else:
            warns.append(f"{rel}: {n}")
    return fails, warns


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
                    + check_duplicate_map_keys(f, raw)
                    + check_ctx_menu_order(raw))
        if problems:
            total += len(problems)
            rel = f.relative_to(ROOT) if ROOT in f.parents or f.is_relative_to(ROOT) else f
            print(f"\n[FAIL] {rel}")
            for p in problems:
                print(f"       - {p}")

    # Whole-tree, so it runs once after every file has been read.
    d4fails, d4warns = check_d4_json_reads(files)
    if d4fails:
        total += len(d4fails)
        print("\n[FAIL] direct d4data JSON reads above baseline")
        for p in d4fails:
            print(f"       - {p}")
    if d4warns and not quiet:
        print("\nverify-src: direct d4data JSON reads (existing debt, not a failure) —")
        for p in d4warns:
            print(f"       · {p}")

    # Settings-key hygiene — three whole-tree checks, same inventory+baseline contract.
    for title, (cfails, cwarns) in (
            ("settings keys written but never read", check_dead_settings_keys(files)),
            ("combos persisted by display text",     check_text_persisted_combos(files)),
            ("character tokens tested by name",      check_character_name_tests(files))):
        if cfails:
            total += len(cfails)
            print(f"\n[FAIL] {title}")
            for p in cfails:
                print(f"       - {p}")
        if cwarns and not quiet:
            print(f"\nverify-src: {title} (reviewed, not a failure) —")
            for p in cwarns:
                print(f"       · {p}")

    if total == 0:
        if not quiet:
            print(f"verify-src: OK — {len(files)} file(s) clean "
                  f"(non-empty, balance, header-only includes, format args, Qt macro names, "
                  f"duplicate lambdas, duplicate map keys, d4data JSON baseline, "
                  f"dead settings keys, combo text persistence, character name tests)")
        return 0
    print(f"\nverify-src: {total} problem(s) in {len(files)} file(s) — fix before building.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
