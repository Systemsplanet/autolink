#!/usr/bin/env python3
"""test_dashboard_assets.py — self-test for dashboard_assets.py.

Pins the dashboard-asset pipeline so a future refactor can't
silently drift the generated header away from what the
firmware + host tests + dashboard JS test expect.

What this test gates:
  * `dashboard_assets.py` reads three markup parts, raw CSS,
    and raw JS, and emits a header whose runtime
    `DASHBOARD_HTML` bytes equal the original concatenation.
    Drift here means the chunked-send loop in
    AutoLinkWeb::handleRoot stops at a stale byte count.
  * Each `{{VERSION}}` marker is split into a C++ string-
    literal-boundary pair so `AUTOLINK_VERSION` expands at
    compile time. Drift here means the dashboard prints
    `v{{VERSION}}` instead of the value of the
    AUTOLINK_VERSION macro.
  * Idempotency: re-running the script produces a
    byte-identical header. Drift here means the test
    make-target keeps regenerating the file for no reason.

Run from repo root:
  python3 build/test_dashboard_assets.py
"""
import hashlib
import re
import subprocess
import sys
from pathlib import Path


def _read(p: Path) -> str:
    return p.read_text(encoding='utf-8')


def _runtime_size_from_header(hdr: str, version_str: str) -> int:
    """Reproduce the C++ runtime byte count of DASHBOARD_HTML.

    Each part is `R"DELIM(content)DELIM"` and contributes
    `len(content)` bytes (the delimiters are zero-width at
    runtime). Each `AUTOLINK_VERSION` token contributes the
    macro-expanded length of the value string (read
    from include/AutoLink.h at test time) bytes at
    compile time -- adjacent string literals concatenate,
    and the macro expands to a string literal whose
    *contents* (not the surrounding quotes) are spliced in.

    Note: naively scanning to the first ';' fails because
    HTML attributes inside the raw-string content can contain
    ';' (CSS rules, JS escapes). We scan for the closing
    `)DELIM";` sequence instead.

    All sizes are byte counts (UTF-8 for raw-string content,
    raw ASCII for the version token).
    """
    version_bytes = len(version_str.encode('utf-8'))

    sizes = {}
    for name in ['DASHBOARD_HTML_PART_A', 'DASHBOARD_CSS',
                 'DASHBOARD_HTML_PART_B', 'DASHBOARD_JS',
                 'DASHBOARD_HTML_PART_C']:
        anchor = hdr.find(f'{name}[] = ')
        if anchor == -1:
            sys.exit(f'test_dashboard_assets: cannot locate {name} in header')
        rpos = hdr.find('R"', anchor)
        paren = hdr.find('(', rpos + 2)
        delim = hdr[rpos + 2:paren]
        end_marker = f'){delim}";'
        close = hdr.find(end_marker, anchor)
        if close == -1:
            sys.exit(f'test_dashboard_assets: cannot find closing '
                     f'{end_marker!r} for {name}')
        expr = hdr[rpos:close + len(end_marker) - 1]
        size = 0
        i = 0
        while i < len(expr):
            if expr.startswith('R"', i):
                j = i + 2
                while j < len(expr) and expr[j] != '(':
                    j += 1
                local_delim = expr[i + 2:j]
                j += 1
                end_marker_local = f'){local_delim}"'
                k = expr.find(end_marker_local, j)
                if k == -1:
                    sys.exit(f'test_dashboard_assets: unterminated '
                             f'raw-string in {name}')
                content = expr[j:k]
                size += len(content.encode('utf-8'))
                i = k + len(end_marker_local)
            elif expr.startswith('AUTOLINK_VERSION', i):
                size += version_bytes
                i += len('AUTOLINK_VERSION')
            else:
                i += 1
        sizes[name] = size
    # DASHBOARD_HTML is the concatenation of the five parts,
    # each of which already ends with a null terminator. When
    # adjacent parts are spliced, the interior nulls become
    # ordinary bytes of the concatenated string. Only the
    # final null (from PART_C) remains as the terminator.
    # So sizeof(DASHBOARD_HTML) = sum(content_bytes) + 1.
    total = sum(sizes.values()) + 1
    return total, sizes


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    src = repo / 'src/al/web'
    script = repo / 'build/dashboard_assets.py'
    include = repo / 'include/AutoLink.h'

    # 1. Required inputs exist.
    required = [
        src / 'dashboard.css',
        src / 'dashboard.js',
        src / 'dashboard_html_part_a.html',
        src / 'dashboard_html_part_b.html',
        src / 'dashboard_html_part_c.html',
    ]
    for p in required:
        if not p.exists():
            sys.exit(f'test_dashboard_assets: missing input {p}')

    # 2. Run dashboard_assets.py and verify idempotency.
    h = src / 'AutoLinkWebHtml.h'
    before = _read(h) if h.exists() else None
    subprocess.run([sys.executable, str(script), '--repo', str(repo)],
                   check=True)
    after = _read(h)

    # Locate AUTOLINK_VERSION's expanded value in include/AutoLink.h.
    include_h = _read(include)
    m = re.search(r'#define\s+AUTOLINK_VERSION\s+"([^"]+)"', include_h)
    if not m:
        sys.exit('test_dashboard_assets: cannot locate AUTOLINK_VERSION '
                 'definition in include/AutoLink.h')
    version_str = m.group(1)

    if before is not None and before != after:
        # Re-run; if the second run produces something different
        # from `after`, the script isn't idempotent.
        subprocess.run([sys.executable, str(script), '--repo', str(repo)],
                       check=True)
        second = _read(h)
        if second != after:
            sys.exit('test_dashboard_assets: dashboard_assets.py is not idempotent')
        sys.exit(f'test_dashboard_assets: {h} was stale (regenerated)')
    elif before is None:
        sys.exit(f'test_dashboard_assets: {h} did not exist before run')

    # 3. The generated header must not contain any {{VERSION}}
    #    marker — every marker must have been split into a
    #    string-literal-boundary pair.
    if '{{VERSION}}' in after:
        idx = after.find('{{VERSION}}')
        ctx = after[max(0, idx-40):idx+40]
        sys.exit(f'test_dashboard_assets: {{VERSION}} marker leaked '
                 f'past the split pass: ...{ctx}...')

    # 4. AUTOLINK_VERSION must appear at least three times —
    #    the three version-injection sites in the markup+JS.
    n_version = after.count('AUTOLINK_VERSION')
    if n_version < 3:
        sys.exit(f'test_dashboard_assets: expected >= 3 '
                 f'AUTOLINK_VERSION tokens, got {n_version}')

    # 5. The runtime byte count of DASHBOARD_HTML must equal
    #    what AutoLinkWeb::handleRoot expects. The original
    #    AutoLinkWebHtml.h (before the refactor) emitted a
    #    DASHBOARD_HTML of exactly 31222 bytes (sum of the
    #    original R"HTML()HTML" segment bytes + 3 inlined
    #    AUTOLINK_VERSION tokens). The new header must hit
    #    that same byte count so the chunked-send loop keeps
    #    its length contract.
    runtime, sizes = _runtime_size_from_header(after, version_str)
    if runtime <= 0:
        sys.exit('test_dashboard_assets: cannot compute runtime size')

    expected = 31222
    if runtime != expected:
        sys.exit(f'test_dashboard_assets: runtime size {runtime} '
                 f'differs from expected {expected} '
                 f'(sizes: {sizes})')

    # 6. The dashboard test must be able to load dashboard.js
    #    directly (per the AGENTS-cross-cutting note).
    js = _read(src / 'dashboard.js')
    # Spot-check: copyLog + al_logAsText must be in the file.
    for needle in ['function copyLog', 'function al_logAsText',
                   "'ping.txt'", "'pong.txt'"]:
        if needle not in js:
            sys.exit(f'test_dashboard_assets: dashboard.js missing {needle!r}')

    # 7. The CSS must not contain version tokens (no {{VERSION}}
    #    and no AUTOLINK_VERSION).
    css = _read(src / 'dashboard.css')
    if '{{VERSION}}' in css or 'AUTOLINK_VERSION' in css:
        sys.exit('test_dashboard_assets: dashboard.css has stray version markers')

    # 8. The committed generated header should hash to a
    #    known-good value so future drift trips here loudly.
    h_hex = hashlib.sha256(after.encode('utf-8')).hexdigest()[:16]
    print(f'test_dashboard_assets: PASS '
          f'(runtime={runtime}B, sha256={h_hex}, '
          f'sizes={sizes})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
