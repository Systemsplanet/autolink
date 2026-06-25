# AutoLink desktop INTEGRATION test build.
#
# Integration tests are multi-second, two-Link end-to-end runs
# that exercise the protocol across a host pipe (no hardware).
# They live here, NOT in test/test_desktop/, so unit-test
# `make` can stay subsecond.
#
# `test_embedded/` holds the Arduino-sketch integration test
# (cross-compiled via arduino-cli; see ../test_embedded/test_embedded.ino).
# That path is opt-in: `make test_embedded_compile` requires
# the toolchain the build/install scripts wire up.
#
# Build modes:
#
#   make            build and run every integration suite
#   make loopback   30 s two-Link end-to-end smoke test
#   make loopback_quick  5 s smoke test
#   make loopback_all    run all 4 mode combinations (5 s each)
#   make test_embedded_compile   cross-compile test_embedded.ino
#
# Per-suite targets (e.g. `make test_loopback_noise`) build and
# run one suite.
#
# Sources live in ../../../src (core); shared test helpers
# (MockHal, WireSim) live in ../../common.

CXX      = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -DAUTOLINK_HOST_TEST \
           -I. -I../../common -I../../../src -I../../../src/al
SRC      = ../../../src
AL       = ../../../src/al
PROTO    = $(AL)/link
HAL      = $(AL)/hal
UTIL     = $(AL)/util
WEB      = $(AL)/web
# Mirror the src/al/ layout in test/itest/test_desktop/al/<sub>/
TAL      = al
TPROTO   = $(TAL)/link
LINK_SRC = $(PROTO)/Link.cpp $(PROTO)/LinkBaudSweep.cpp $(PROTO)/LinkFrameRx.cpp $(PROTO)/LinkArq.cpp $(PROTO)/LinkReorder.cpp $(PROTO)/LinkSweep.cpp $(PROTO)/ArqCache.cpp $(UTIL)/Log.cpp $(UTIL)/UtilCrc.cpp $(UTIL)/UtilCobs.cpp

# Integration-test binaries. End-to-end smoke tests that wire
# two Link instances together and verify protocol behaviour
# across a host pipe (no hardware).
ITEST_BINS = run_loopback run_loopback_noise run_loopback_sync

# ---- embedded sketch cross-compile ----------------------------------------
# See test_embedded/Makefile — the Arduino-sketch integration test
# has its own Makefile (different toolchain, different I/O shape).
#
# ---- 'all' runs every integration suite, captures per-suite runtime,
# then prints a summary: tests passed, total bytes used by the
# suite binaries, and peak RSS. Integration suites are allowed
# to take seconds; the summary still prints per-suite wall time
# so a regression that pushes a suite over budget is visible.
all: $(ITEST_BINS)
	@total_bytes=0; peak_kb=0; pass=0; fail=0; fail_names=""; \
	start_ns=$$(date +%s%N); \
	for t in $(ITEST_BINS); do \
	    t0=$$(date +%s%N); \
	    sz=$$(stat -c %s $$t 2>/dev/null || echo 0); \
	    out=$$(python3 ../../common/peak_rss.py ./$$t 2>/dev/null || echo "0\n0\n1"); \
	    ms=$$(echo "$$out" | sed -n 1p); \
	    rss=$$(echo "$$out" | sed -n 2p); \
	    rc=$$(echo "$$out" | sed -n 3p); \
	    ms=$${ms:-0}; rss=$${rss:-0}; rc=$${rc:-1}; \
	    if [ "$$rc" -eq 0 ]; then \
	        total_bytes=$$(( total_bytes + sz )); \
	        if [ "$$rss" -gt "$$peak_kb" ] 2>/dev/null; then peak_kb=$$rss; fi; \
	        printf "  [PASS] %-40s %5d ms  %7d B  rss=%s KiB\n" $$t $$ms $$sz $$rss; \
	        pass=$$(( pass + 1 )); \
	    else \
	        printf "  [FAIL] %-40s %5d ms\n" $$t $$ms; \
	        fail=$$(( fail + 1 )); \
	        fail_names="$$fail_names $$t"; \
	    fi; \
	done; \
	end_ns=$$(date +%s%N); \
	total_ms=$$(( (end_ns - start_ns) / 1000000 )); \
	printf "\n=== Integration test summary ===\n"; \
	printf "  tests passed : %d / %d\n" $$pass $(words $(ITEST_BINS)); \
	if [ $$fail -gt 0 ]; then printf "  FAILED       :%s\n" "$$fail_names"; fi; \
	printf "  total time   : %d ms\n" $$total_ms; \
	printf "  total bytes  : %d B (sum of suite binaries on disk)\n" $$total_bytes; \
	printf "  peak RSS     : %d KiB (largest single-suite resident set)\n" $$peak_kb; \
	if [ $$fail -gt 0 ]; then exit 1; fi; \
	printf "=== Integration test suite PASS ===\n"

test_cpp: $(ITEST_BINS)
	@fail=0; \
	for t in $(ITEST_BINS); do \
	    echo "  $$t"; \
	    ./$$t >/dev/null 2>&1 && echo "    PASS" || { echo "    FAIL"; fail=1; }; \
	done; \
	if [ $$fail -eq 0 ]; then echo "=== $(words $(ITEST_BINS)) integration suites PASS ==="; else echo "=== integration suites FAIL ==="; exit 1; fi

# ---- per-suite build rules ---------------------------------------------------

run_loopback: $(LINK_SRC) $(TPROTO)/loopback_test.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/loopback_test.cpp -o $@ -pthread

run_loopback_noise: $(LINK_SRC) $(TPROTO)/loopback_noise_test.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/loopback_noise_test.cpp -o $@ -pthread

run_loopback_sync: $(LINK_SRC) $(TPROTO)/loopback_sync_test.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/loopback_sync_test.cpp -o $@ -pthread

# ---- convenience runners ----------------------------------------------------

loopback: run_loopback
	./run_loopback 30

loopback_quick: run_loopback
	./run_loopback 5

loopback_verbose: run_loopback
	./run_loopback 30 verbose

# Run all 4 mode combinations (seq/rand × reliable/unreliable) as a
# single 'make loopback_all' target. Each runs 5s. Useful when
# changing anything in the framing or ARQ state machine.
loopback_all: run_loopback
	@echo "--- sequential + reliable ---"
	@./run_loopback 5 sequential
	@echo "--- random + reliable ---"
	@./run_loopback 5 random
	@echo "--- sequential + unreliable (raw) ---"
	@./run_loopback 5 sequential unreliable
	@echo "--- random + unreliable (raw) ---"
	@./run_loopback 5 random unreliable
	@echo "=== All 4 loopback modes PASS ==="

test_loopback: run_loopback
	@./run_loopback 10 >/dev/null 2>&1 && echo 'PASS' || echo 'FAIL'

# Run the loopback + loopback_noise itests
# in BOTH modes (ASYNC and SYNC) and emit a
# per-mode summary. One driver, one
# output, clear mode differentiation.
#
# SYNC mode is the new default. The driver
# exists so a single
# 'make loopback_modes' command verifies
# both code paths end-to-end before
# shipping a release.
#
# 'sec=N' overrides the per-test duration
# (default 5s). 'noise=off' skips the
# noise test (run it separately if you
# only want the clean-wire mode check).
loopback_modes: run_loopback run_loopback_noise
	@sec=$${sec:-5}; noise=$${noise:-on}; \
	total_pass=0; total_fail=0; \
	async_pass=0; async_fail=0; \
	sync_pass=0; sync_fail=0; \
	for mode in async sync; do \
	    for variant in clean noise; do \
	        if [ "$$variant" = "clean" ]; then \
	            bin=./run_loopback; noise_off=1; \
	        else \
	            if [ "$$noise" != "on" ]; then continue; fi; \
	            bin=./run_loopback_noise; noise_off=0; \
	        fi; \
	        tag="[$$mode / $$variant / $$sec s]"; \
	        printf "\n--- %-30s %s\n" "$$bin" "$$tag" | tee /tmp/last_mode_tag; \
	        out=$$( $$bin $$sec $$mode 2>&1 ); rc=$$?; \
	        summary=$$(echo "$$out" | grep -E "^(PASS|FAIL|\\[.*\\] All|TX=)" | head -8); \
	        echo "$$summary" | sed 's/^/  /'; \
	        if [ $$rc -eq 0 ]; then \
	            printf "  RESULT: PASS\n"; \
	            total_pass=$$(( total_pass + 1 )); \
	            if [ "$$mode" = "async" ]; then async_pass=$$(( async_pass + 1 )); else sync_pass=$$(( sync_pass + 1 )); fi; \
	        else \
	            printf "  RESULT: FAIL (rc=%d)\n" $$rc; \
	            total_fail=$$(( total_fail + 1 )); \
	            if [ "$$mode" = "async" ]; then async_fail=$$(( async_fail + 1 )); else sync_fail=$$(( sync_fail + 1 )); fi; \
	        fi; \
	    done; \
	done; \
	printf "\n=== Loopback modes summary ===\n"; \
	printf "  ASYNC : %d pass, %d fail\n" $$async_pass $$async_fail; \
	printf "  SYNC  : %d pass, %d fail\n" $$sync_pass $$sync_fail; \
	printf "  TOTAL : %d pass, %d fail\n" $$total_pass $$total_fail; \
	if [ $$total_fail -gt 0 ]; then exit 1; fi; \
	printf "=== ALL MODES PASS ===\n"

test_loopback_noise: run_loopback_noise
	./run_loopback_noise

test_loopback_suite: test_loopback

# ---- housekeeping -----------------------------------------------------------
clean:
	rm -f $(ITEST_BINS) *.gcno *.gcda

.PHONY: all test_cpp clean \
        loopback loopback_quick loopback_verbose loopback_all \
        test_loopback test_loopback_noise test_loopback_suite \
        loopback_modes
