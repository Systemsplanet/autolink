#!/usr/bin/env python3
# Run a command, capture wall time, exit code, and peak RSS (KiB).
# Used by Makefiles to summarize test suite memory use without
# depending on GNU /usr/bin/time.
import os
import resource
import subprocess
import sys
import time

if len(sys.argv) < 2:
    sys.stderr.write("usage: peak_rss.py <cmd> [args...]\n")
    sys.exit(2)

t0 = time.monotonic_ns()
proc = subprocess.run(sys.argv[1:], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
t1 = time.monotonic_ns()
ru = resource.getrusage(resource.RUSAGE_CHILDREN)

# Print three lines so the Makefile can `read` them reliably:
#   <wall_ms>
#   <peak_rss_kib>
#   <exit_code>
sys.stdout.write(f"{(t1 - t0) // 1000000}\n")
sys.stdout.write(f"{ru.ru_maxrss}\n")
sys.stdout.write(f"{proc.returncode}\n")
sys.exit(proc.returncode)
