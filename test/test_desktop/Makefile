# AutoLink desktop UNIT test build.
#
# Unit tests only — small, subsecond, no wall-clock sleeps, no
# multi-second runs. Two-Link loopback / noise tests live under
# test/itest/test_desktop/ because they take seconds of wall time
# and exercise the protocol end-to-end across two instances.
#
# Build modes:
#
#   make            build and run every suite (default; must stay subsecond)
#   make test_asan  build every suite with AddressSanitizer + UBSan
#   make coverage   build every suite with --coverage instrumentation
#
# Per-suite targets (e.g. `make test_crc`) build and run one suite.
#
# Sources live in ../../src (core) and ../../src/util (utilities); both are
# on the include path so headers resolve by the same names the firmware uses.
# Shared test infrastructure (MockHal, WireSim) lives in ../common.

CXX      = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -DAUTOLINK_HOST_TEST \
           -I. -I../common -I../../src -I../../src/al
SRC      = ../../src
AL       = ../../src/al
PROTO    = $(AL)/link
HAL      = $(AL)/hal
UTIL     = $(AL)/util
WEB      = $(AL)/web
PONG     = $(AL)/pingpong
# Mirror the src/al/ layout in test/test_desktop/al/<sub>/
TAL      = al
THAL     = $(TAL)/hal
TPROTO   = $(TAL)/link
TUTIL    = $(TAL)/util
TWEB     = $(TAL)/web
LINK_SRC = $(PROTO)/Link.cpp $(PROTO)/LinkBaudSweep.cpp $(PROTO)/LinkFrameRx.cpp $(UTIL)/Log.cpp $(UTIL)/UtilCrc.cpp $(UTIL)/UtilCobs.cpp
AUTOLINK_SRC = $(SRC)/AutoLink.cpp $(LINK_SRC)

# Unit-test binaries only. Loopback / loopback_noise live under
# test/itest/test_desktop/ (see ../itest/test_desktop/Makefile).
TEST_BINS = run_test_crc run_test_cobs run_test_blink run_test_framerx \
            run_test_baudsweep run_test_log run_test_mockhal run_test_autolink \
            run_test_alink_io run_test_alink_message            run_test_alink_error run_test_alink_cobsseq \
            run_test_alink_arq run_test_alink_web \
            run_test_alink_facade run_test_wiresim_closedloop \
            run_test_clock_injection run_test_linkdecision \
            run_test_alink_v53 run_test_alink_v531 \
            run_test_alink_v531_never_leave_p1 \
            run_test_linkreorder \
            run_test_esp_idf_error_etiquette

# 'all' runs every unit suite, captures per-suite runtime, then
# prints a summary: tests passed, total bytes used by the suite
# binaries (proxy for resident memory on disk), and peak RSS as
# observed via /usr/bin/time. Unit tests should remain subsecond
# end-to-end; the per-suite wall time is printed so any regression
# that pushes a suite over the budget is visible immediately.
all: $(TEST_BINS)
	@total_bytes=0; peak_kb=0; pass=0; fail=0; fail_names=""; \
	start_ns=$$(date +%s%N); \
	for t in $(TEST_BINS); do \
	    t0=$$(date +%s%N); \
	    sz=$$(stat -c %s $$t 2>/dev/null || echo 0); \
	    out=$$(python3 ../common/peak_rss.py ./$$t 2>/dev/null || echo "0\n0\n1"); \
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
	printf "\n=== Unit test summary ===\n"; \
	printf "  tests passed : %d / %d\n" $$pass $(words $(TEST_BINS)); \
	if [ $$fail -gt 0 ]; then printf "  FAILED       :%s\n" "$$fail_names"; fi; \
	printf "  total time   : %d ms\n" $$total_ms; \
	printf "  total bytes  : %d B (sum of suite binaries on disk)\n" $$total_bytes; \
	printf "  peak RSS     : %d KiB (largest single-suite resident set)\n" $$peak_kb; \
	if [ $$fail -gt 0 ]; then exit 1; fi; \
	if [ $$total_ms -gt 60000 ]; then \
	    printf "  WARNING      : unit suite exceeded 60 s wall budget — see rule in AGENTS.md\n"; \
	fi; \
	printf "=== Unit test suite PASS ===\n"

# C++-only gate: build + run every suite. Use this when you
# need a green/red signal independent of dashboard JS.
test_cpp: $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
	    echo "  $$t"; \
	    ./$$t >/dev/null 2>&1 && echo "    PASS" || { echo "    FAIL"; fail=1; }; \
	done; \
	if [ $$fail -eq 0 ]; then echo "=== $(words $(TEST_BINS)) C++ suites PASS ==="; else echo "=== C++ suites FAIL ==="; exit 1; fi

# ---- per-suite build rules ---------------------------------------------------

test_crc: run_test_crc
	./run_test_crc

test_cobs: run_test_cobs
	./run_test_cobs

test_blink: run_test_blink
	./run_test_blink

test_framerx: run_test_framerx
	./run_test_framerx

test_baudsweep: run_test_baudsweep
	./run_test_baudsweep

test_log: run_test_log
	./run_test_log

test_mockhal: run_test_mockhal
	./run_test_mockhal

test_autolink: run_test_autolink \
        test_alink_web
	./run_test_autolink \
        test_alink_web

test_alink_io: run_test_alink_io
	./run_test_alink_io

test_alink_message: run_test_alink_message
	./run_test_alink_message


test_alink_error: run_test_alink_error
	./run_test_alink_error


test_alink_cobsseq: run_test_alink_cobsseq
	./run_test_alink_cobsseq

run_test_alink_cobsseq: $(LINK_SRC) $(TPROTO)/LinkCobsSeqTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/LinkCobsSeqTest.cpp -o $@

run_test_alink_arq: $(AUTOLINK_SRC) $(TPROTO)/LinkArqTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/LinkArqTest.cpp -o $@

run_test_linkdecision: $(TPROTO)/LinkDecisionTest.cpp
	$(CXX) $(CXXFLAGS) $(TPROTO)/LinkDecisionTest.cpp -o $@

run_test_alink_web: $(LINK_SRC) $(WEB)/AutoLinkWebCore.cpp $(TWEB)/AutoLinkWebTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(WEB)/AutoLinkWebCore.cpp $(TWEB)/AutoLinkWebTest.cpp -o $@

run_test_alink_v53: $(LINK_SRC) $(TPROTO)/LinkV53Test.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/LinkV53Test.cpp -o $@

run_test_alink_v531: $(LINK_SRC) $(TPROTO)/LinkV531Test.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/LinkV531Test.cpp -o $@

run_test_alink_v531_never_leave_p1: $(LINK_SRC) $(TPROTO)/LinkV531NeverLeaveP1Test.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/LinkV531NeverLeaveP1Test.cpp -o $@

run_test_linkreorder: $(LINK_SRC) $(TPROTO)/LinkReorderTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/LinkReorderTest.cpp -o $@

# Static-analysis test for ESP-IDF error-etiquette — reads
# the .cpp/.h source files and asserts every known ESP-IDF
# call site has a paired esp_err_to_name log.
run_test_esp_idf_error_etiquette: EspIdfErrorEtiquetteTest.cpp
	$(CXX) $(CXXFLAGS) EspIdfErrorEtiquetteTest.cpp -o $@

run_test_crc: $(UTIL)/UtilCrc.cpp $(TUTIL)/UtilCrcTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilCrc.cpp $(TUTIL)/UtilCrcTest.cpp -o $@

run_test_cobs: $(UTIL)/UtilCobs.cpp $(TUTIL)/UtilCobsTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilCobs.cpp $(TUTIL)/UtilCobsTest.cpp -o $@

run_test_blink: $(UTIL)/UtilBlink.h $(TUTIL)/UtilBlinkTest.cpp
	$(CXX) $(CXXFLAGS) $(TUTIL)/UtilBlinkTest.cpp -o $@

run_test_framerx: $(PROTO)/LinkFrameRx.cpp $(TPROTO)/LinkFrameRxTest.cpp
	$(CXX) $(CXXFLAGS) $(PROTO)/LinkFrameRx.cpp $(UTIL)/UtilCobs.cpp $(UTIL)/UtilCrc.cpp $(TPROTO)/LinkFrameRxTest.cpp -o $@

run_test_baudsweep: $(PROTO)/LinkBaudSweep.cpp $(TPROTO)/LinkBaudSweepTest.cpp
	$(CXX) $(CXXFLAGS) $(PROTO)/LinkBaudSweep.cpp $(TPROTO)/LinkBaudSweepTest.cpp -o $@

run_test_log: $(UTIL)/Log.cpp $(TUTIL)/LogTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/Log.cpp $(TUTIL)/LogTest.cpp -o $@

run_test_mockhal: $(LINK_SRC) $(THAL)/MockHalTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(THAL)/MockHalTest.cpp -o $@

run_test_autolink: $(AUTOLINK_SRC) AutoLinkTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) AutoLinkTest.cpp -o $@

run_test_alink_facade: $(AUTOLINK_SRC) AutoLinkFacadeTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) AutoLinkFacadeTest.cpp -o $@

run_test_wiresim_closedloop: $(AUTOLINK_SRC) WireSimClosedLoopTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) WireSimClosedLoopTest.cpp -o $@

# : deterministic clock-injection tests. Pins the three
# time-dependent protocol paths (idle timeout, ACK RTO, sweep stall)
# as sub-ms host tests via MockHal::pumpClock.
run_test_clock_injection: $(AUTOLINK_SRC) ClockInjectionTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) ClockInjectionTest.cpp -o $@

run_test_alink_io: $(LINK_SRC) $(TPROTO)/LinkIOTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/LinkIOTest.cpp -o $@

run_test_alink_message: $(AUTOLINK_SRC) $(TPROTO)/LinkMessageTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/LinkMessageTest.cpp -o $@

run_test_alink_error: $(LINK_SRC) $(TPROTO)/LinkErrorTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/LinkErrorTest.cpp -o $@

# ---- integration: AddressSanitizer + UBSan ----------------------------------
# Catches the same categories valgrind would catch (out-of-bounds, use-after-
# free, leaks) but is gcc-native and orders of magnitude faster. Run after
# `make clean` to drop the non-instrumented binaries.
ASAN_FLAGS  = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
ASAN_LIBS   = -fsanitize=address,undefined

asan: CXXFLAGS += $(ASAN_FLAGS)
asan: LDLIBS    = $(ASAN_LIBS)
asan: clean all

test_asan: asan
	@echo "=== ASan + UBSan run complete (no diagnostics = pass) ==="

# ---- integration: gcov coverage --------------------------------------------
# Builds with --coverage, runs every suite (so .gcda files are written),
# then runs gcov on each .gcno file to produce annotated coverage.
COV_FLAGS = -fprofile-arcs -ftest-coverage -fbranch-probabilities -O0
COV_LIBS  = -lgcov

coverage: CXXFLAGS += $(COV_FLAGS)
coverage: LDLIBS    = $(COV_LIBS)
coverage: clean
	@$(MAKE) all CXXFLAGS="$(CXXFLAGS)" LDLIBS="$(LDLIBS)"
	@echo "=== Running every test suite to gather .gcda files ==="
	@for t in $(TEST_BINS); do \
	    echo "  $$t"; \
	    ./$$t > /dev/null 2>&1 || { echo "  $$t FAILED"; exit 1; }; \
	done
	@echo "=== Merging .gcda counts from every binary ==="
	@./coverage_merge.sh
	@echo "--- coverage summary (lines / branches) ---" | tee coverage/coverage.txt
	@for g in coverage/*.gcov; do \
	    base=$$(basename $$g .gcov); \
	    awk -v f=$$base 'BEGIN{lines=hit=bhit=btot=0} \
	        /^[[:space:]]*[#0-9-]+:[[:space:]]*[0-9]+:/ { \
	            tag=$$1; gsub(/:/, "", tag); gsub(/[[:space:]]/, "", tag); \
	            if (tag == "-") next;  # non-executable line, skip \
	            lines++; \
	            if (tag+0 > 0) hit++; \
	        } \
	        /^branch[[:space:]]+[0-9]+/ { \
	            btot++; \
	            if ($$0 !~ /never[[:space:]]+executed/) bhit++; \
	        } \
	        END { printf "  %-40s %5d/%-5d lines (%5.1f%%)  %3d/%-3d branches\n", \
	            f, hit, lines, lines?hit*100.0/lines:0, bhit, btot }' $$g; \
	done | sort -k4 -nr | tee -a coverage/coverage.txt
	@echo "=== Coverage report in coverage/coverage.txt ==="

# ---- housekeeping -----------------------------------------------------------
clean:
	rm -f $(TEST_BINS) *.gcno *.gcda
	rm -rf coverage

.PHONY: all asan test_asan coverage clean test_cpp \
        test_crc test_cobs test_blink test_framerx test_baudsweep test_log \
        test_mockhal test_alink_io test_alink_message \
        test_alink_error test_alink_cobsseq test_autolink \
        test_alink_web test_alink_v53 test_alink_v531 \
        test_alink_v531_never_leave_p1 test_wiresim_closedloop \
        test_esp_idf_error_etiquette

test_alink_web: run_test_alink_web
	./run_test_alink_web

test_alink_v53: run_test_alink_v53
	./run_test_alink_v53

test_alink_v531: run_test_alink_v531
	./run_test_alink_v531

test_alink_v531_never_leave_p1: run_test_alink_v531_never_leave_p1
	./run_test_alink_v531_never_leave_p1

# ---- dashboard JS test (Node + jsdom) ------------------------------------
# Verifies the embedded dashboard JavaScript (poll(), bindLvlGroup(),
# role-conditional UI, etc) without an ESP32. Requires Node + jsdom.
#
# This target is OPT-IN. The C++ test gate (`make`, `make test_cpp`)
# does NOT depend on it. To bootstrap jsdom, run once with network:
#   make test_dashboard_js_bootstrap
# Then any subsequent `make test_dashboard_js` runs the JS test
# (skipping cleanly if node or jsdom is still unavailable). This
# keeps `make` green/red a function of the C++ suite, not of npm
# reachability.
JS_TEST = dashboard-js-test.js
JS_NODE_MODULES = node_modules/jsdom

test_dashboard_js:
	@if [ ! -x "$$(command -v node)" ]; then \
	    echo "  SKIP: node not in PATH"; \
	elif [ ! -d node_modules/jsdom ]; then \
	    echo "  SKIP: jsdom not installed (run 'make test_dashboard_js_bootstrap' once with network)"; \
	else \
	    node $(JS_TEST); \
	fi

# One-time bootstrap. Only invoke this when you have network
# access — `make` and `make test_cpp` do NOT depend on it.
test_dashboard_js_bootstrap: package.json
	@npm install
	@touch $(JS_NODE_MODULES)

package.json:
	@echo '{ "name": "autolink-test", "version": "0.0.0", "private": true, "dependencies": { "jsdom": "*" } }' > $@

.PHONY: test_dashboard_js test_dashboard_js_bootstrap
