#!/usr/bin/env python3
"""version.py -- manage docs/Version.md for AutoLink.

Single source of truth for the Version.md structural invariant:
"the file holds at most N entries, newest on top." Replaces the
hand-trimmed "drop the entry manually" mistake (and the awk
filter that produced bogus paths because it was filtering
unzip -l output instead of the file list directly).

Subcommands:

  trim [--keep N]            Enforce the invariant. Drop oldest
                             entries beyond N. Default N is the
                             value pinned in AGENTS.md. Idempotent.

  add   --version V          Insert a new entry at the top, then
        [--title "..."]      trim. The entry body is a
                             placeholder scaffold the author
                             fills in (description, What moved,
                             Why, Wire format, Regression
                             coverage, Disclosed limitations,
                             Result). The script does NOT write
                             the substantive content.

  check [--keep N]           Read-only. Exit non-zero if the
                             file has more than N entries, or
                             if entries are out of order, or if
                             the header / separator structure is
                             malformed. Designed for CI / the
                             pre-zip gate.

Why split into trim / add / check:

  - trim enforces the structural invariant. It's pure
    mechanics; the script never reads the body of an entry.
  - add scaffolds the new entry's skeleton and runs trim.
    It does NOT understand what the entry should say. The
    author does. This keeps the script from accidentally
    publishing boilerplate ("test hooks moved off the public
    API") that doesn't match the actual change.
  - check is what CI runs. It catches drift before the zip
    goes out (e.g., someone hand-edits the file and forgets
    to trim, or orders entries wrong).

Usage:

  build/version.py trim                      # trim to default N (20)
  build/version.py trim --keep 8             # trim to 8 (legacy pin)
  build/version.py add --version <X.Y.Z>
  build/version.py add --version <X.Y.Z> --title "fix description"
  build/version.py check                     # CI gate
  build/version.py check --keep 20
"""

import argparse
import re
import sys
from pathlib import Path

# Default N — pinned to AGENTS.md "Trim docs/Version.md" rule.
# Bump this in lockstep with AGENTS.md.
DEFAULT_KEEP = 20

# Path to the canonical version doc, relative to project root.
DEFAULT_PATH = 'docs/Version.md'

# Required file header. We refuse to operate on a file that
# doesn't start with this, so a typo in --path can't silently
# mangle an unrelated markdown file.
_REQUIRED_HEADER = (
    '# 📅 AutoLink Version History\n'
    '\n'
    'All releases, most recent first.\n'
)

# Entry separator between two consecutive version blocks.
# The blank line after `---` is part of the format.
_SEPARATOR = '\n---\n\n'

# Entry header regex. Captures the version string for ordering
# and for the new-entry scaffold.
_ENTRY_RE = re.compile(r'^## (v\d+\.\d+\.\d+)\b', re.MULTILINE)

# Template scaffolded by `add`. The author replaces the
# <fill-me-in> markers with the actual content. The shell
# version (the four # bullets) is what AGENTS.md rule 5 asks
# for: description, fix, regression test, disclosed limitations.
# The Result bullet is added in practice; we include it so the
# scaffold matches the existing entries' shape.
_ADD_TEMPLATE = """## {version}

**{title}**

<one-paragraph description: what changed and why. Match the
voice of the existing entries — terse, specific, no marketing.

Replace this whole <fill-me-in> block with the entry body.>
"""

# Sentinel the script leaves at the bottom of the file after
# trim, so a final entry still has a trailing separator line
# matching the prior format.
_FINAL_SEP = '\n---\n'


def _semver_key(v: str) -> tuple[int, int, int]:
    """Parse 'X.Y.Z' -> (X, Y, Z) for ordering checks."""
    m = re.match(r'^v(\d+)\.(\d+)\.(\d+)$', v)
    if not m:
        raise ValueError(f'not a semver string: {v!r}')
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)))


def parse(path: Path) -> tuple[str, list[str], str]:
    """Split the doc into (header, [entry_bodies], footer).

    entry_bodies are in file order (newest first). Each body is
    the chunk starting at `## vX.Y.Z` and ending just before
    the next `## v` or the trailing `---`. The trailing
    `---\n` is the footer.

    Raises if the header doesn't match or separators are
    malformed."""
    raw = path.read_text(encoding='utf-8')
    if not raw.startswith(_REQUIRED_HEADER):
        raise SystemExit(
            f'version.py: {path} does not start with the required '
            f'header.\n  expected prefix: {_REQUIRED_HEADER!r}\n'
            f'  got:               {raw[:80]!r}')
    body = raw[len(_REQUIRED_HEADER):]
    # Split into entry chunks. Each entry starts with `## v`.
    # The separator is `\n---\n\n` between entries.
    chunks = re.split(r'(?m)(?=^## v\d+\.\d+\.\d+\b)', body)
    # The first chunk is whatever precedes the first `## v`.
    # That's normally the body of the first entry's prelude if
    # the file starts cleanly. We expect it to be empty (the
    # header ends with `\n\n` and the first thing is `## v...`).
    if chunks and chunks[0].strip() == '':
        chunks = chunks[1:]
    if not chunks:
        raise SystemExit(f'version.py: {path} has no version entries')
    # Validate each chunk starts with `## vX.Y.Z`.
    parsed: list[str] = []
    for c in chunks:
        c_stripped = c.rstrip()
        # Drop the trailing `\n---\n` if present; we'll re-add
        # the canonical separator on emit.
        if c_stripped.endswith('---'):
            c_stripped = c_stripped[:-3].rstrip()
        if not _ENTRY_RE.match(c_stripped):
            raise SystemExit(
                f'version.py: malformed entry chunk in {path}:\n'
                f'  {c_stripped[:80]!r}')
        parsed.append(c_stripped)
    return _REQUIRED_HEADER, parsed, ''


def check_order(entries: list[str]) -> None:
    """Raise if entries are not strictly descending by version."""
    versions = [_ENTRY_RE.match(e).group(1) for e in entries]
    keys = [_semver_key(v) for v in versions]
    for a, b in zip(keys, keys[1:]):
        if a <= b:
            raise SystemExit(
                f'version.py: entries out of order: '
                f'{versions[keys.index(b) - 1] if False else ""}'
                f'expected newest first, found {a} <= {b}')


def trim(path: Path, keep: int) -> int:
    """Drop oldest entries beyond `keep`. Returns the number
    of entries dropped. Idempotent."""
    if keep < 1:
        raise SystemExit(f'version.py: --keep must be >= 1, got {keep}')
    header, entries, _ = parse(path)
    check_order(entries)
    if len(entries) <= keep:
        return 0
    dropped = len(entries) - keep
    keep_entries = entries[:keep]
    # Re-emit with the canonical separator.
    out = header + _SEPARATOR.join(keep_entries) + _FINAL_SEP
    path.write_text(out, encoding='utf-8')
    return dropped


def add(path: Path, version: str, title: str | None) -> None:
    """Insert a new entry at the top. Body is a scaffold the
    author must fill in. Refuses to proceed if the new version
    isn't strictly greater than the current top entry."""
    if not version.startswith('v'):
        version = 'v' + version
    header, entries, _ = parse(path)
    check_order(entries)
    top_version = _ENTRY_RE.match(entries[0]).group(1)
    if _semver_key(version) <= _semver_key(top_version):
        raise SystemExit(
            f'version.py: refusing to add {version}; current top is '
            f'{top_version}. New entries must be strictly greater.')
    if title is None:
        title = '<one-line title: what changed>'
    scaffold = _ADD_TEMPLATE.format(version=version, title=title)
    new_entries = [scaffold] + entries
    out = header + _SEPARATOR.join(new_entries) + _FINAL_SEP
    path.write_text(out, encoding='utf-8')


def check(path: Path, keep: int) -> int:
    """Read-only validation. Returns 0 on success, non-zero on
    any structural problem."""
    try:
        header, entries, _ = parse(path)
    except SystemExit as e:
        print(e, file=sys.stderr)
        return 1
    try:
        check_order(entries)
    except SystemExit as e:
        print(e, file=sys.stderr)
        return 1
    if len(entries) > keep:
        print(
            f'version.py: {path} has {len(entries)} entries; '
            f'--keep is {keep}. Run `build/version.py trim '
            f'--keep {keep}` to drop the oldest.',
            file=sys.stderr)
        return 1
    print(f'version.py: {path} OK '
          f'({len(entries)} entries, --keep={keep})')
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description='Manage docs/Version.md structural invariants.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument('--path', default=DEFAULT_PATH,
                    help=f'path to Version.md (default: {DEFAULT_PATH})')
    sub = ap.add_subparsers(dest='cmd', required=True)

    p_trim = sub.add_parser('trim', help='enforce --keep N (drop oldest)')
    p_trim.add_argument('--keep', type=int, default=DEFAULT_KEEP,
                        help=f'number of entries to keep '
                             f'(default: {DEFAULT_KEEP})')

    p_add = sub.add_parser('add', help='scaffold a new entry at the top')
    p_add.add_argument('--version', required=True,
                       help='new version string, e.g. <X.Y.Z>')
    p_add.add_argument('--title', default=None,
                       help='one-line title for the entry')
    p_add.add_argument('--keep', type=int, default=DEFAULT_KEEP,
                       help=f'number of entries to keep after add '
                            f'(default: {DEFAULT_KEEP})')

    p_check = sub.add_parser('check',
                             help='read-only structural check (for CI)')
    p_check.add_argument('--keep', type=int, default=DEFAULT_KEEP,
                         help=f'expected max entries '
                              f'(default: {DEFAULT_KEEP})')

    args = ap.parse_args()
    path = Path(args.path)
    if not path.exists():
        print(f'version.py: file not found: {path}', file=sys.stderr)
        return 2

    if args.cmd == 'trim':
        dropped = trim(path, args.keep)
        print(f'version.py: trim --keep {args.keep} '
              f'-> dropped {dropped} entries.')
        return 0
    if args.cmd == 'add':
        add(path, args.version, args.title)
        dropped = trim(path, args.keep)
        print(f'version.py: added v{args.version}; '
              f'trim dropped {dropped} older entries.')
        return 0
    if args.cmd == 'check':
        return check(path, args.keep)
    return 1


if __name__ == '__main__':
    sys.exit(main())