#!/usr/bin/env python3
"""test_pretty_print.py -- self-contained test of pretty_print.py.

Tests pretty_print()'s gate (only C/C++ source files get
clang-format; everything else is a no-op) and the report
shape.

Covers:

  FILE-LEVEL (pretty_print() round trip):
    1. .cpp file -- clang-format runs.
    2. .h file -- clang-format runs.
    3. .ino file -- clang-format runs.
    4. .py file -- skipped (not C/C++).
    5. .md file -- skipped.
    6. .sh file -- skipped.
    7. .txt file -- skipped.
    8. Non-existent file -- ok=False, file-not-found note.
    9. .c, .cc, .cxx, .hh, .hpp, .hxx all accepted.
"""
import os
import subprocess
import sys
import tempfile

SCRIPT = os.path.join(os.path.dirname(__file__) or '.', 'pretty_print.py')

sys.path.insert(0, os.path.dirname(__file__) or '.')
import pretty_print as pp  # noqa: E402


PASS_COUNT = 0
FAIL_COUNT = 0


def _ok(name, ok, **detail):
    global PASS_COUNT, FAIL_COUNT
    if ok:
        PASS_COUNT += 1
        print(f'  PASS  {name}')
    else:
        FAIL_COUNT += 1
        print(f'  FAIL  {name}')
        for k, v in detail.items():
            print(f'    {k}: {v!r}')


def run_pp_on_text(text, suffix='.cpp'):
    with tempfile.TemporaryDirectory() as td:
        p = os.path.join(td, 'a' + suffix)
        with open(p, 'w') as f:
            f.write(text)
        r = pp.pretty_print(p)
        r['final_content'] = open(p).read() if os.path.exists(p) else None
        return r


# ---------------------------------------------------------------------------
# C/C++ files DO get clang-format
# ---------------------------------------------------------------------------
print('C/C++ files -- clang-format runs:')

r = run_pp_on_text('int f(){return 0;}\n', suffix='.cpp')
_ok('1. .cpp file: clang-format runs (formatted=True)',
   r['ok'] and r['formatted'],
   report=r)

r = run_pp_on_text('#pragma once\nint f();\n', suffix='.h')
_ok('2. .h file: clang-format runs (formatted=True)',
   r['ok'] and r['formatted'],
   report=r)

r = run_pp_on_text('void setup(){}\nvoid loop(){}\n', suffix='.ino')
_ok('3. .ino file: clang-format runs (formatted=True)',
   r['ok'] and r['formatted'],
   report=r)

# ---------------------------------------------------------------------------
# Non-C/C++ files are SKIPPED
# ---------------------------------------------------------------------------
print('\nNon-C/C++ files -- skipped:')

r = run_pp_on_text('#!/usr/bin/env python3\nx = 1\n', suffix='.py')
_ok('4. .py file: skipped (not C/C++)',
   r['ok'] and not r['formatted']
   and 'not a C/C++' in r['notes'][0]
   and r['final_content'].startswith('#!/usr/bin/env python3\n'),
   report=r)

r = run_pp_on_text('# Title\n\nSome text.\n', suffix='.md')
_ok('5. .md file: skipped (not C/C++)',
   r['ok'] and not r['formatted']
   and 'not a C/C++' in r['notes'][0]
   and r['final_content'] == '# Title\n\nSome text.\n',
   report=r)

r = run_pp_on_text('#!/bin/bash\necho hi\n', suffix='.sh')
_ok('6. .sh file: skipped (not C/C++)',
   r['ok'] and not r['formatted']
   and 'not a C/C++' in r['notes'][0],
   report=r)

r = run_pp_on_text('hello world\n', suffix='.txt')
_ok('7. .txt file: skipped (not C/C++)',
   r['ok'] and not r['formatted']
   and 'not a C/C++' in r['notes'][0],
   report=r)

# ---------------------------------------------------------------------------
# Error path
# ---------------------------------------------------------------------------
print('\nError path:')

r = pp.pretty_print('/tmp/does_not_exist_xyz_123.cpp')
_ok('8. non-existent file: ok=False, file-not-found note',
   not r['ok'] and 'not found' in r['notes'][0],
   report=r)

# ---------------------------------------------------------------------------
# Other C/C++ extensions
# ---------------------------------------------------------------------------
print('\nOther C/C++ extensions:')
for suffix in ['.c', '.cc', '.cxx', '.hh', '.hpp', '.hxx']:
    r = run_pp_on_text('int f();\n', suffix=suffix)
    _ok(f'9. .{suffix} file: clang-format runs',
       r['ok'] and r['formatted'],
       suffix=suffix, report=r)


# ---------------------------------------------------------------------------
# CLI smoke test
# ---------------------------------------------------------------------------
print('\nCLI smoke test:')
with tempfile.TemporaryDirectory() as td:
    p = os.path.join(td, 'a.cpp')
    with open(p, 'w') as f:
        f.write('int f(){return 0;}\n')
    out = subprocess.run(
        ['python3', SCRIPT, p],
        capture_output=True, text=True)
    if out.returncode == 0:
        with open(p) as f:
            content = f.read()
        if 'formatted' in out.stdout:
            PASS_COUNT += 1
            print('  PASS  CLI runs clang-format on .cpp')
        else:
            FAIL_COUNT += 1
            print(f'  FAIL  CLI stdout missing "formatted": {out.stdout!r}')
    else:
        FAIL_COUNT += 1
        print(f'  FAIL  CLI exit: {out.returncode}, stderr: {out.stderr}')

# CLI on a .py file should be a no-op (exit 0, "no changes").
with tempfile.TemporaryDirectory() as td:
    p = os.path.join(td, 'a.py')
    with open(p, 'w') as f:
        f.write('x = 1\n')
    out = subprocess.run(
        ['python3', SCRIPT, p],
        capture_output=True, text=True)
    if out.returncode == 0 and 'no changes' in out.stdout:
        PASS_COUNT += 1
        print('  PASS  CLI on .py: exit 0, no changes')
    else:
        FAIL_COUNT += 1
        print(f'  FAIL  CLI on .py: exit={out.returncode}, '
              f'stdout={out.stdout!r}')


# ---------------------------------------------------------------------------
print(f'\n{"="*50}')
print(f'Total: {PASS_COUNT} passed, {FAIL_COUNT} failed')
if FAIL_COUNT > 0:
    sys.exit(1)
