#!/usr/bin/env python3
"""pretty_print.py -- single canonical pretty-printer for the project.

A thin wrapper around `clang-format -i` (using the project's
.clang-format, which has BreakStringLiterals: false set).

That is the whole tool. There is no string-merge pass, no
backup step, no verification -- `clang-format` is
deterministic and reversible via `git revert` if it ever
breaks. The project root's `.clang-format` already has
`BreakStringLiterals: false` set, so no new string-literal
splits are introduced going forward; any legacy splits have
already been cleaned up in earlier versions.

The `.clang-format` also sets
`AllowShortFunctionsOnASingleLine: All`, so single-statement
bodies (and bodies short enough to fit on one line) collapse
to a single line. The default `Linux` brace style is
preserved for multi-statement bodies.

clang-format is only run on C/C++ source files (.c, .cc,
.cpp, .cxx, .h, .hh, .hpp, .hxx, .ino). Running it on
Python or Markdown would mangle those files since
clang-format would parse them as C++.

Usage:
  pretty_print.py file1.cpp [file2.h ...]
"""
import subprocess
import sys
from pathlib import Path


_C_CPP_SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh',
                   '.hpp', '.hxx', '.ino'}

# Files that are the output of a code generator and must
# not be reformatted. The generator's byte output is the
# single source of truth for the file's contents; clang-format
# has no semantic context to apply here and will only re-wrap
# long raw-string-literal declarations across multiple lines,
# which then fails the generator's --check contract (the
# generator writes one-line, clang-format writes two-line).
# Add to this set whenever a new generated header joins the
# tree. The entries are matched against the absolute path
# suffix so the same filename under a different directory
# still matches.
_GENERATED_HEADERS = (
    'src/al/web/AutoLinkWebHtml.h',
)


def _is_generated(p: Path) -> bool:
    s = str(p.resolve()) if p.exists() else str(p)
    return any(s.endswith(e) or e in s for e in _GENERATED_HEADERS)


def _is_c_cpp(p: Path) -> bool:
    return p.suffix.lower() in _C_CPP_SUFFIXES


def _have_clang_format() -> bool:
    return (
        subprocess.run(
            ['which', 'clang-format'],
            capture_output=True).returncode == 0
    )


def _user_clang_format_path() -> str:
    """Return the absolute path where a `pip install --user`
    would land the clang-format wrapper, even if that path
    is not on the current PATH. Empty string if no HOME
    is set (shouldn't happen on supported platforms)."""
    home = subprocess.run(
        ['python3', '-c', 'import os; print(os.path.expanduser("~/.local/bin"))'],
        capture_output=True, text=True).stdout.strip()
    candidate = f'{home}/clang-format'
    if (subprocess.run(
            ['test', '-x', candidate],
            capture_output=True).returncode == 0):
        return candidate
    return ''


def _have(cmd: str) -> bool:
    return (
        subprocess.run(
            ['which', cmd],
            capture_output=True).returncode == 0
    )


def _install_clang_format() -> bool:
    """Try to install clang-format with the
    platform package manager. Returns True
    on success. Best-effort: apt, brew, and
    pip are attempted in that order (apt +
    brew are the GitHub Actions Ubuntu and
    macOS dev box channels; pip is the
    fallback for sandboxes and minimal
    containers where the apt repo doesn't
    carry clang-format). Other platforms
    (Alpine, Nix, Windows) fall through and
    the user must install it manually.

    For apt-get we try `sudo` first
    (interactive dev box), then fall
    back to no-sudo (CI runner, root
    container, sandbox). For brew we
    always go without sudo (brew
    doesn't need it). For pip we try
    the system install first, then
    `--break-system-packages` for
    PEP 668-managed distros (Debian
    12+, Ubuntu 23.04+) that refuse
    the default. The pip installs use
    `--no-warn-script-location` and we
    verify the resulting binary is on
    PATH after each attempt -- `pip
    install` returns rc=0 even when
    it places the script in a
    directory that is NOT on PATH
    (e.g. `~/.local/bin` for a
    `--user` install running as root),
    so we don't trust the return code
    alone.
    """
    if _have_clang_format():
        return True
    cmds = []
    if _have('apt-get'):
        cmds += [
            ['sudo', '-n', 'apt-get', 'install', '-y', 'clang-format'],
            ['apt-get', 'install', '-y', 'clang-format'],
        ]
    if _have('brew'):
        cmds += [['brew', 'install', 'clang-format']]
    if _have('pip') or _have('pip3'):
        pip = 'pip3' if _have('pip3') else 'pip'
        cmds += [
            [pip, 'install', '--break-system-packages', 'clang-format'],
            [pip, 'install', 'clang-format'],
            [pip, 'install', '--user', '--break-system-packages', 'clang-format'],
            [pip, 'install', '--user', 'clang-format'],
        ]
    for cmd in cmds:
        try:
            r = subprocess.run(
                cmd, capture_output=True, text=True,
                timeout=120)
            if r.returncode != 0:
                continue
            if _have_clang_format():
                return True
            user_path = _user_clang_format_path()
            if user_path:
                import os
                os.environ['PATH'] = (
                    f'{os.path.dirname(user_path)}:{os.environ.get("PATH", "")}')
                if _have_clang_format():
                    return True
        except (FileNotFoundError, OSError,
                subprocess.TimeoutExpired):
            continue
    return _have_clang_format()


def _clang_format(path: Path) -> str:
    """Run clang-format -i on `path` using the project's
    .clang-format. Returns an error message ('' on success,
    'skipped: ...' if the tool isn't available after a
    best-effort install attempt)."""
    if not _have_clang_format():
        if not _install_clang_format():
            return 'skipped: clang-format not installed'
    try:
        result = subprocess.run(
            ['clang-format', '-i', str(path)],
            capture_output=True, text=True, timeout=10)
        if result.returncode == 0:
            return ''
        return f'clang-format failed: {result.stderr[:500]}'
    except FileNotFoundError:
        return 'skipped: clang-format not installed'
    except subprocess.TimeoutExpired:
        return 'skipped: clang-format timed out'


def pretty_print(path: str) -> dict:
    """Run clang-format on one file. Returns a report dict
    with formatted flag and any notes (errors or
    skipped-tool messages)."""
    p = Path(path)
    report = {'path': str(p), 'formatted': False,
              'ok': True, 'notes': []}

    if not p.exists():
        report['ok'] = False
        report['notes'].append('file not found')
        return report

    if not _is_c_cpp(p):
        report['notes'].append('skipped: not a C/C++ source file')
        return report

    if _is_generated(p):
        report['notes'].append(
            'skipped: generated header (canonical bytes owned by '
            'its generator, clang-format must not reformat)')
        return report

    err = _clang_format(p)
    if err:
        report['notes'].append(err)
    else:
        report['formatted'] = True

    return report


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f'usage: {sys.argv[0]} FILE [FILE ...]',
              file=sys.stderr)
        sys.exit(1)
    total_ok = 0
    total_fail = 0
    total_formatted = 0
    for path in sys.argv[1:]:
        r = pretty_print(path)
        if r['ok']:
            total_ok += 1
            if r['formatted']:
                total_formatted += 1
            status = 'formatted' if r['formatted'] else 'no changes'
            extras = ' [' + '; '.join(r['notes']) + ']' if r['notes'] else ''
            print(f'  {r["path"]}: {status}{extras}')
        else:
            total_fail += 1
            print(f'  {r["path"]}: FAIL -- {r["notes"][0]}')
    print(f'\n{total_ok} files OK ({total_formatted} formatted), '
          f'{total_fail} failed')
    sys.exit(1 if total_fail > 0 else 0)
