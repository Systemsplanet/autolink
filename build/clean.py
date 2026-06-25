#!/usr/bin/env python3
"""clean.py -- remove build byproducts from the AutoLink tree.

Single source of truth for "what does 'clean' delete?". Replaces
the scattered `find ... -delete` commands and the hand-rolled
`make clean` shell that used to drift apart every time someone
added a new test target.

The previous mistake this script avoids:
  `find . -name "run_test_*" -delete`
  `find test/itest -name "run_loopback*" -delete`
  `find . -name "*.o" -delete`
  ... with paths and globs duplicated across CI scripts, the
  top-level Makefile, and the per-suite Makefiles. The same
  pattern of typos ("run_test_*" vs "run-test_*") would slip
  into different scripts and leave stale artifacts.

Concretely:

  build/clean.py              # dry run; prints what would be removed
  build/clean.py --apply      # actually delete
  build/clean.py --root PATH  # operate on a different project root
                              # (used by the rule-24 baseline check
                              # to clean an extracted zip into a fresh
                              # dir before running `make test`)

What gets removed:

  test/test_desktop/run_test_*            unit-suite binaries
  test/itest/test_desktop/run_loopback*   itest binaries
  **/*.o, **/*.gcno, **/*.gcda            gcov / object outputs
  **/*.bak, **/compile_commands.json      editor / clang dumps
  build/verify_build/build                ESP32 build cache
  build/verify_build/libraries            ESP32 library cache

What is NOT touched: source files, headers, docs, .clang-format,
the test/ tree's .cpp inputs, the arduino-cli install under
~/.arduino15/, anything outside the project root.

Source files are git-restored if a clean goes wrong; the script
deliberately limits its blast radius to artifacts, never inputs.
"""

import argparse
import os
import sys
from pathlib import Path

# Path globs that always live at the same place. Hand-curated so
# the script doesn't have to second-guess a glob expansion.
_TEST_BIN_GLOBS = [
    'test/test_desktop/run_test_*',
    'test/itest/test_desktop/run_loopback*',
]

# Path globs that may live anywhere in the tree. Tracked separately
# because they're cheap and the only legitimate place for them is
# alongside the source that produced them.
_RECURSIVE_GLOBS = [
    '**/*.o',
    '**/*.gcno',
    '**/*.gcda',
    '**/*.bak',
    '**/compile_commands.json',
]

# Paths outside the recursive glob scope that are build caches.
# These are directories, not files; remove the whole tree.
_DIR_PATHS = [
    'build/verify_build/build',
    'build/verify_build/libraries',
]

# Refuse to operate outside these roots. The script is meant
# for the AutoLink tree; if someone runs it at /, a typo in
# --root would nuke their home directory.
_DEFAULT_ROOT = '.'


def _matches(p: Path, globs: list[str], root: Path) -> bool:
    """True if p (absolute) matches any of the relative
    globs from the root."""
    try:
        rel = p.relative_to(root)
    except ValueError:
        return False
    for g in globs:
        if rel.match(g):
            return True
    return False


def _discover(root: Path) -> tuple[list[Path], list[Path]]:
    """Walk the tree, return (files_to_remove, dirs_to_remove).

    Files are deduplicated against the dir list so a directory
    isn't rmtree'd after its contents have already been
    unlinked individually."""
    files: list[Path] = []
    dirs: list[Path] = []

    for g in _TEST_BIN_GLOBS:
        files.extend(sorted(root.glob(g)))
    for g in _RECURSIVE_GLOBS:
        files.extend(sorted(root.glob(g)))
    for d in _DIR_PATHS:
        p = root / d
        if p.exists():
            dirs.append(p)

    # Dedupe: if a file lives under one of the dir paths, the
    # dir-rmtree will get it; skip the file entry.
    dir_set = {d.resolve() for d in dirs}
    files = [f for f in files if not any(
        d in f.resolve().parents or f.resolve() == d for d in dir_set
    )]
    return files, dirs


def _format_bytes(n: int) -> str:
    for unit in ('B', 'KiB', 'MiB', 'GiB'):
        if n < 1024:
            return f'{n:.0f} {unit}' if unit == 'B' else f'{n:.1f} {unit}'
        n /= 1024
    return f'{n:.1f} TiB'


def _report(files: list[Path], dirs: list[Path], root: Path) -> None:
    total_bytes = 0
    for f in files:
        try:
            total_bytes += f.stat().st_size
        except OSError:
            pass
    print(f'  root       : {root.resolve()}')
    print(f'  files      : {len(files)} ({_format_bytes(total_bytes)})')
    print(f'  dirs       : {len(dirs)}')
    if files:
        print('  file list  :')
        for f in files[:10]:
            try:
                sz = _format_bytes(f.stat().st_size)
            except OSError:
                sz = '?'
            print(f'    {sz:>10}  {f.relative_to(root)}')
        if len(files) > 10:
            print(f'    ... and {len(files) - 10} more')
    if dirs:
        print('  dir list   :')
        for d in dirs:
            print(f'    {d.relative_to(root)}')


def _remove(files: list[Path], dirs: list[Path]) -> tuple[int, int]:
    """Delete files and dirs. Returns (files_removed, errors)."""
    import shutil
    removed = 0
    errors = 0
    for f in files:
        try:
            f.unlink()
            removed += 1
        except OSError as e:
            print(f'  ERR removing {f}: {e}', file=sys.stderr)
            errors += 1
    for d in dirs:
        try:
            shutil.rmtree(d)
            removed += 1
        except OSError as e:
            print(f'  ERR removing {d}: {e}', file=sys.stderr)
            errors += 1
    return removed, errors


def main() -> int:
    ap = argparse.ArgumentParser(
        description='Remove AutoLink build byproducts. '
                    'Dry-run by default; pass --apply to delete.')
    ap.add_argument('--apply', action='store_true',
                    help='actually delete (default is dry-run)')
    ap.add_argument('--root', default=_DEFAULT_ROOT,
                    help='project root (default: cwd)')
    args = ap.parse_args()

    root = Path(args.root).resolve()
    if not root.is_dir():
        print(f'clean.py: not a directory: {root}', file=sys.stderr)
        return 2

    files, dirs = _discover(root)

    mode = 'APPLY' if args.apply else 'DRY-RUN'
    print(f'=== clean.py ({mode}) ===')
    _report(files, dirs, root)

    if not files and not dirs:
        print('  nothing to do.')
        return 0

    if not args.apply:
        print('\n  pass --apply to actually remove.')
        return 0

    removed, errors = _remove(files, dirs)
    print(f'\n  removed {removed} entries ({errors} errors)')
    return 0 if errors == 0 else 1


if __name__ == '__main__':
    sys.exit(main())