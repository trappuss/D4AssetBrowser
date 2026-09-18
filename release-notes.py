#!/usr/bin/env python3
"""Extract one version's section out of CHANGELOG.md into a release-body file.

    python release-notes.py 2.3.0 dist\\RELEASE_BODY.md

Why this exists as a real file rather than a PowerShell one-liner inside a .bat:
the one-liner it replaces wrote a ONE-BYTE body for 2.3.0 and said nothing, and
`if not exist` waved that through, so the release was published empty and the only
symptom was on the website. A file can be read, tested and given exit codes.

Exit codes
    0   wrote the file
    2   wrong arguments
    3   CHANGELOG.md missing or unreadable
    4   no section for that version
    5   the section is too small to be a real changelog entry

Output is UTF-8 with NO byte-order mark and LF endings. A BOM ends up in the
release body as a stray character on the first line; gh does not strip it.
"""

# Annotations stay strings, so `str | None` and `list[str]` do not have to be
# evaluated at import time -- those spellings are a TypeError on Python 3.9 and
# the build box's Python is whatever happens to be on PATH.
from __future__ import annotations

import pathlib
import re
import sys

MIN_BYTES = 32          # anything shorter is a failure wearing a file's clothes


def extract(text: str, version: str) -> str | None:
    """The body under `## <version>`, up to the next `## ` heading or end of file.

    Tolerates a `v` prefix on the heading and trailing whitespace after it. The
    lookahead needs the space after ## so that `### Fixed` does not end the
    section -- the sub-headings inside an entry all start with ###.
    """
    pattern = (
        r'^##[ \t]+v?' + re.escape(version) + r'[ \t]*$'
        r'\n(.*?)'
        r'(?=^##[ \t]|\Z)'
    )
    m = re.search(pattern, text, re.MULTILINE | re.DOTALL)
    return m.group(1).strip('\n') if m else None


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print(__doc__.strip().splitlines()[2].strip())
        return 2

    version, out_path = argv[1], argv[2]
    src = pathlib.Path('CHANGELOG.md')

    try:
        # utf-8-sig drops a BOM if the file has one; normalising CRLF first means
        # the `$` anchors land where they look like they land.
        text = src.read_bytes().decode('utf-8-sig').replace('\r\n', '\n')
    except OSError as e:
        print(f'release-notes: cannot read {src}: {e}')
        return 3

    body = extract(text, version)
    if body is None:
        print(f'release-notes: no "## {version}" section in {src}')
        heads = re.findall(r'^##[ \t]+(\S+)', text, re.MULTILINE)[:6]
        if heads:
            print('release-notes: sections found: ' + ', '.join(heads))
        return 4

    data = body.encode('utf-8')
    if len(data) < MIN_BYTES:
        print(f'release-notes: the "## {version}" section is only {len(data)} '
              f'byte(s) -- refusing to write it as a release body')
        return 5

    out = pathlib.Path(out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)            # write_bytes, never write_text: no re-encoding
    print(f'release-notes: {len(data)} bytes -> {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
