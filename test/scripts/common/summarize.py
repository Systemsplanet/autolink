#!/usr/bin/env python3
"""Combine per-suite `[PASS]`/`[FAIL]` lines from two log files
into a single summary block. Used by the top-level test/Makefile
to aggregate `make test` and `make itest` results.

Usage:
    summarize.py <unit_log> <itest_log>

Prints a 6-line block:
    unit tests      : X passed, Y failed
    itest suites    : X passed, Y failed
    total tests     : X passed, Y failed
    total bytes     : N B (sum of all suite binaries on disk)
    peak memory     : K KiB (largest single-suite resident set)
    total wall time : N ms

Exit 0 if total_fail == 0, else 1.
"""
import re
import sys

PASS = re.compile(r"\[PASS\]")
FAIL = re.compile(r"\[FAIL\]")
RSS = re.compile(r"rss=(\d+) KiB")
BYTES = re.compile(r"\b(\d+) B\b")
WALL_MS = re.compile(r"total time\s*:\s*(\d+) ms")

def count(path: str) -> tuple[int, int, int, int]:
    """Return (pass, fail, max_rss_kib, total_bytes)."""
    p = f = rss = b = 0
    with open(path) as fh:
        for line in fh:
            if PASS.search(line):
                p += 1
            if FAIL.search(line):
                f += 1
            m = RSS.search(line)
            if m:
                rss = max(rss, int(m.group(1)))
            for m in BYTES.finditer(line):
                b += int(m.group(1))
    return p, f, rss, b

def wall_ms(path: str) -> int:
    with open(path) as fh:
        for line in fh:
            m = WALL_MS.search(line)
            if m:
                return int(m.group(1))
    return 0

def main() -> int:
    if len(sys.argv) != 3:
        sys.stderr.write("usage: summarize.py <unit_log> <itest_log>\n")
        return 2
    up, uf, ur, ub = count(sys.argv[1])
    ip, if_, ir, ib = count(sys.argv[2])
    tp = up + ip
    tf = uf + if_
    total_bytes = ub + ib
    peak_kib = max(ur, ir)
    wall = wall_ms(sys.argv[1]) + wall_ms(sys.argv[2])
    print("=== Combined test summary ===")
    print(f"  unit tests      : {up} passed, {uf} failed")
    print(f"  itest suites    : {ip} passed, {if_} failed")
    print(f"  total tests     : {tp} passed, {tf} failed")
    print(f"  total bytes     : {total_bytes} B (sum of all suite binaries on disk)")
    print(f"  peak memory     : {peak_kib} KiB (largest single-suite resident set)")
    print(f"  total wall time : {wall} ms")
    return 1 if tf > 0 else 0

if __name__ == "__main__":
    sys.exit(main())
