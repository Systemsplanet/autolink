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
# Test-side scripts (peak_rss, coverage, env) live in ../scripts/{common,coverage,env}.

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
TARQ     = $(TPROTO)/arq
TSWEEP   = $(TPROTO)/sweep
TUTIL    = $(TAL)/util
TWEB     = $(TAL)/web
TPING    = $(TAL)/pingpong
LINK_SRC = $(PROTO)/LinkCore.cpp $(PROTO)/LinkTx.cpp $(PROTO)/LinkRx.cpp \
           $(PROTO)/LinkSweep.cpp $(PROTO)/LinkTimers.cpp $(PROTO)/LinkApi.cpp \
           $(PROTO)/LinkFrameRx.cpp $(PROTO)/LinkReorder.cpp \
           $(PROTO)/arq/ArqCache.cpp $(PROTO)/arq/LinkArq.cpp \
           $(PROTO)/sweep/LinkBaudSweep.cpp $(PROTO)/sweep/LinkSweep.cpp \
           $(UTIL)/Log.cpp $(UTIL)/UtilCrc.cpp $(UTIL)/UtilCobs.cpp
AUTOLINK_SRC = $(SRC)/AutoLink.cpp $(LINK_SRC)

# Unit-test binaries only. Loopback / loopback_noise live under
# test/itest/test_desktop/ (see ../itest/test_desktop/Makefile).
dashboard_assets_check:
	@cd ../.. && python3 build/dashboard_assets.py --check || \
	    (echo 'AutoLinkWebHtml.h is stale (dashboard_assets.py --check failed).' && \
	     echo 'Run `python3 build/dashboard_assets.py` to regenerate, then re-run the suite.' && \
	     exit 1)

TEST_BINS = run_test_crc run_test_cobs run_test_blink run_test_framerx \
            run_test_baudsweep run_test_log run_test_mockhal run_test_autolink \
            run_test_alink_io run_test_alink_message_roundtrip \
            run_test_alink_message_corrupt \
            run_test_alink_message_resync \
            run_test_alink_message_edge \
            run_test_alink_error run_test_alink_cobsseq \
            run_test_alink_arq run_test_alink_web \
            run_test_alink_facade run_test_wiresim_closedloop \
            run_test_clock_injection run_test_linkdecision \
            run_test_alink_baud_preference \
            run_test_alink_sweep_phase \
            run_test_alink_sweep_p1_guard \
            run_test_linkreorder \
            run_test_esp_idf_error_etiquette \
            run_test_compile_check \
            run_test_arq_cache \
            run_test_sync_mode \
            run_test_ping_resume_source \
            run_test_pingpong_structure \
            run_test_pingpong_log_hygiene \
            run_test_accessor_structure \
            run_test_version_free_source \
            run_test_linkcontext \
            run_test_uri_handler_alignment \
            run_test_handle_root_chunked \
            run_test_web_begin_lifecycle \
            run_test_web_httpd_retry \
            run_test_link_begin_defer \
            run_test_httpd_startup \
            run_test_httpd_reset_endpoint \
            run_test_esphal_begin_and_health \
            run_test_swp_phase_single_source \
            run_test_pong_phase2_entry \
            run_test_phase2_dwell_floor \
            run_test_pong_p1_guard_outlasts_master_p2 \
            run_test_mode_toggle_ui \
            run_test_onbreak_guard \
            run_test_boundary_invariants

# 'all' runs every unit suite, captures per-suite runtime, then
# prints a summary: tests passed, total bytes used by the suite
# binaries (proxy for resident memory on disk), and peak RSS as
# observed via /usr/bin/time. Unit tests should remain subsecond
# end-to-end; the per-suite wall time is printed so any regression
# that pushes a suite over the budget is visible immediately.
all: dashboard_assets_check $(TEST_BINS)
	@total_bytes=0; peak_kb=0; pass=0; fail=0; fail_names=""; \
	start_ns=$$(date +%s%N); \
	for t in $(TEST_BINS); do \
	    t0=$$(date +%s%N); \
	    sz=$$(stat -c %s $$t 2>/dev/null || echo 0); \
	    out=$$(python3 ../scripts/common/peak_rss.py ./$$t 2>/dev/null || echo "0\n0\n1"); \
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

test_autolink: run_test_autolink
	./run_test_autolink

test_alink_io: run_test_alink_io
	./run_test_alink_io

test_alink_message: run_test_alink_message_roundtrip run_test_alink_message_corrupt run_test_alink_message_resync run_test_alink_message_edge
	./run_test_alink_message_roundtrip
	./run_test_alink_message_corrupt
	./run_test_alink_message_resync
	./run_test_alink_message_edge

test_alink_message_roundtrip: run_test_alink_message_roundtrip
	./run_test_alink_message_roundtrip

test_alink_message_corrupt: run_test_alink_message_corrupt
	./run_test_alink_message_corrupt

test_alink_message_resync: run_test_alink_message_resync
	./run_test_alink_message_resync

test_alink_message_edge: run_test_alink_message_edge
	./run_test_alink_message_edge


test_alink_error: run_test_alink_error
	./run_test_alink_error


test_alink_cobsseq: run_test_alink_cobsseq
	./run_test_alink_cobsseq

run_test_alink_cobsseq: $(LINK_SRC) $(TPROTO)/LinkCobsSeqTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/LinkCobsSeqTest.cpp -o $@

run_test_alink_arq: $(LINK_SRC) $(TARQ)/LinkArqTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TARQ)/LinkArqTest.cpp -o $@

run_test_linkdecision: $(TSWEEP)/LinkDecisionTest.cpp
	$(CXX) $(CXXFLAGS) $(TSWEEP)/LinkDecisionTest.cpp -o $@

run_test_alink_web: $(LINK_SRC) $(WEB)/AutoLinkWebCore.cpp $(TWEB)/AutoLinkWebTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(WEB)/AutoLinkWebCore.cpp $(TWEB)/AutoLinkWebTest.cpp -o $@

run_test_alink_baud_preference: $(LINK_SRC) $(TSWEEP)/LinkBaudPreferenceTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TSWEEP)/LinkBaudPreferenceTest.cpp -o $@

run_test_alink_sweep_phase: $(LINK_SRC) $(TSWEEP)/LinkSweepPhaseTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TSWEEP)/LinkSweepPhaseTest.cpp -o $@

run_test_alink_sweep_p1_guard: $(LINK_SRC) $(TSWEEP)/LinkSweepP1GuardTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TSWEEP)/LinkSweepP1GuardTest.cpp -o $@

run_test_linkreorder: $(LINK_SRC) $(TPROTO)/LinkReorderTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/LinkReorderTest.cpp -o $@

# Static-analysis test for ESP-IDF error-etiquette — reads
# the .cpp/.h source files and asserts every known ESP-IDF
# call site has a paired esp_err_to_name log.
run_test_esp_idf_error_etiquette: al/EspIdfErrorEtiquetteTest.cpp
	$(CXX) $(CXXFLAGS) al/EspIdfErrorEtiquetteTest.cpp -o $@

run_test_compile_check: al/CompileCheckTest.cpp
	$(CXX) $(CXXFLAGS) al/CompileCheckTest.cpp -o $@

run_test_arq_cache: $(AUTOLINK_SRC) al/link/arq/ArqCacheTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) al/link/arq/ArqCacheTest.cpp -o $@

run_test_sync_mode: $(AUTOLINK_SRC) al/SyncModeTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) al/SyncModeTest.cpp -o $@

# Source-level regression test for Ping::setPaused resume
# contract. Ping is #ifdef ARDUINO so we can't
# construct one on host; we pin the proactive clearQueue_()
# call by reading the source.
run_test_ping_resume_source: $(TPING)/PingResumeSourceTest.cpp
	$(CXX) $(CXXFLAGS) $(TPING)/PingResumeSourceTest.cpp -o $@

# PingPongBase split into free functions +
# discrete setup steps. Source-level pin
# (Ping is #ifdef ARDUINO so no host coverage).
run_test_pingpong_structure: $(TPING)/PingPongStructureTest.cpp
	$(CXX) $(CXXFLAGS) $(TPING)/PingPongStructureTest.cpp -o $@

# Source-level pins for the Ping / Pong / Link.cpp log-hygiene
# fixes: paused-aware "not ready" line, flushRx-after-clearQueue
# on got<0 + mismatch, Pong's send-failed demoted to warning,
# Link.cpp's WIRING? one-shot. Ping / Pong / Link.cpp are
# `#ifdef ARDUINO` and can't run on host, so this gates the
# bugs by reading the source.
run_test_pingpong_log_hygiene: $(TPING)/PingPongLogHygieneTest.cpp
	$(CXX) $(CXXFLAGS) $(TPING)/PingPongLogHygieneTest.cpp -o $@

# test_* / *_ForTest hooks moved off
# Link / AutoLink public API into
# LinkTestAccessor / AutoLinkTestAccessor
# shims under test/common/. Source-level
# pin: reads Link.h and AutoLink.h, asserts
# the public sections no longer declare
# the hooks and the friend declarations
# for the shims are present.
run_test_accessor_structure: $(LINK_SRC) $(AUTOLINK_SRC) $(TPROTO)/TestAccessorStructureTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/TestAccessorStructureTest.cpp -o $@

run_test_version_free_source: al/VersionFreeSourceTest.cpp
	$(CXX) $(CXXFLAGS) al/VersionFreeSourceTest.cpp -o $@

# LinkArq / LinkReorder / LinkSweep drive the link through
# LinkContext — not friendship. Source-level pin (no friend
# declarations in Link.h, no #include "al/link/Link.h" in any
# helper) plus a runtime mock-context that the helpers compile
# against. Compile-only does most of the work: if a future change
# reverts the helper signatures to Link&, this test stops
# compiling because MockCtx isn't a Link.
run_test_linkcontext: $(PROTO)/LinkCore.cpp $(PROTO)/LinkTx.cpp $(PROTO)/LinkRx.cpp \
                      $(PROTO)/LinkSweep.cpp $(PROTO)/LinkTimers.cpp $(PROTO)/LinkApi.cpp \
                      $(PROTO)/LinkFrameRx.cpp $(PROTO)/LinkReorder.cpp \
                      $(PROTO)/arq/LinkArq.cpp $(PROTO)/sweep/LinkBaudSweep.cpp $(PROTO)/sweep/LinkSweep.cpp \
                      $(UTIL)/Log.cpp $(UTIL)/UtilCrc.cpp $(UTIL)/UtilCobs.cpp \
                      $(TPROTO)/LinkContextTest.cpp
	$(CXX) $(CXXFLAGS) $(PROTO)/LinkCore.cpp $(PROTO)/LinkTx.cpp $(PROTO)/LinkRx.cpp \
		$(PROTO)/LinkSweep.cpp $(PROTO)/LinkTimers.cpp $(PROTO)/LinkApi.cpp \
		$(PROTO)/LinkFrameRx.cpp $(PROTO)/LinkReorder.cpp \
		$(PROTO)/arq/LinkArq.cpp $(PROTO)/sweep/LinkBaudSweep.cpp $(PROTO)/sweep/LinkSweep.cpp \
		$(UTIL)/Log.cpp $(UTIL)/UtilCrc.cpp $(UTIL)/UtilCobs.cpp \
		$(TPROTO)/LinkContextTest.cpp -o $@

# Source-level pin for AutoLinkWeb's httpd handler
# registration: cfg.max_uri_handlers == number of
# declared handlers, and PATHS[] is parallel to URIS[]
# so the failure log names the correct path. AutoLinkWeb
# is `#ifdef ARDUINO` and can't run on host, so this
# gates the bug by reading the source. Covers the
# HANDLERS_FULL failure mode where the dashboard
# silently drops /reboot at startup and 404s it at
# runtime.
run_test_uri_handler_alignment: $(TWEB)/UriHandlerAlignmentTest.cpp
	$(CXX) $(CXXFLAGS) $(TWEB)/UriHandlerAlignmentTest.cpp -o $@

# Source-level pin for AutoLinkWeb::handleRoot's chunked-send
# contract. AutoLinkWeb.cpp is `#ifdef ARDUINO` and can't run
# on host; this gates the bug by reading the source. Covers
# the silent-truncation failure mode where the ~28 KB
# dashboard HTML is shipped in a single `httpd_resp_send`
# call against a 4096-byte httpd send buffer and the
# browser receives a malformed / empty page.
run_test_handle_root_chunked: $(TWEB)/HandleRootChunkedTest.cpp
	$(CXX) $(CXXFLAGS) $(TWEB)/HandleRootChunkedTest.cpp -o $@

run_test_web_begin_lifecycle: $(TWEB)/WebBeginLifecycleTest.cpp
	$(CXX) $(CXXFLAGS) $(TWEB)/WebBeginLifecycleTest.cpp -o $@

run_test_web_httpd_retry: $(TWEB)/WebHttpdRetryTest.cpp
	$(CXX) $(CXXFLAGS) $(TWEB)/WebHttpdRetryTest.cpp -o $@

run_test_link_begin_defer: $(TWEB)/LinkBeginDeferTest.cpp
	$(CXX) $(CXXFLAGS) $(TWEB)/LinkBeginDeferTest.cpp -o $@

# Source-level pin for the httpd startup race and the
# TIME_WAIT retry budget. AutoLinkWeb.cpp is `#ifdef ARDUINO`
# so it can't run on host; this gates the bug by reading
# the source. Covers:
#   - Bug 1: double-entry race between begin() and
#     wifiTaskThunk_() on setupHttpAndLogging_().
#   - Bug 2: HTTPD_RETRY_MAX * HTTPD_RETRY_PRE_MS must
#     cover lwIP TCP TIME_WAIT (~60 s).
#   - Bug 3: uart_driver_install ESP_FAIL recovery —
#     EspHal::begin() must pre-clear with
#     uart_is_driver_installed + uart_driver_delete.
run_test_httpd_startup: $(TWEB)/HttpdStartupTest.cpp
	$(CXX) $(CXXFLAGS) $(TWEB)/HttpdStartupTest.cpp -o $@

# End-to-end HTTP test for the /reset endpoint. Stands up
# a loopback TCP server in a thread that mirrors
# AutoLinkWeb::handleReset()'s contract (resetStats +
# resetErrors + resetDiag + emit "ok" with text/plain),
# then drives a real HTTP POST /reset via the system
# curl. Verifies the body equals "ok" and the handler
# is path-scoped (POST /garbage does NOT fire reset).
# Caps total wall time at 5 s via curl --max-time and
# pthread_join timeout, per AGENTS rule 4.
run_test_httpd_reset_endpoint: $(AUTOLINK_SRC) $(TWEB)/HttpdResetEndpointTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TWEB)/HttpdResetEndpointTest.cpp -o $@ -pthread

run_test_crc: $(UTIL)/UtilCrc.cpp $(TUTIL)/UtilCrcTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilCrc.cpp $(TUTIL)/UtilCrcTest.cpp -o $@

run_test_cobs: $(UTIL)/UtilCobs.cpp $(TUTIL)/UtilCobsTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilCobs.cpp $(TUTIL)/UtilCobsTest.cpp -o $@

run_test_blink: $(UTIL)/UtilBlink.h $(TUTIL)/UtilBlinkTest.cpp
	$(CXX) $(CXXFLAGS) $(TUTIL)/UtilBlinkTest.cpp -o $@

run_test_framerx: $(PROTO)/LinkFrameRx.cpp $(TPROTO)/LinkFrameRxTest.cpp
	$(CXX) $(CXXFLAGS) $(PROTO)/LinkFrameRx.cpp $(UTIL)/UtilCobs.cpp $(UTIL)/UtilCrc.cpp $(TPROTO)/LinkFrameRxTest.cpp -o $@

run_test_baudsweep: $(PROTO)/sweep/LinkBaudSweep.cpp $(TSWEEP)/LinkBaudSweepTest.cpp
	$(CXX) $(CXXFLAGS) $(PROTO)/sweep/LinkBaudSweep.cpp $(TSWEEP)/LinkBaudSweepTest.cpp -o $@

run_test_log: $(UTIL)/Log.cpp $(TUTIL)/LogTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/Log.cpp $(TUTIL)/LogTest.cpp -o $@

run_test_mockhal: $(LINK_SRC) $(THAL)/MockHalTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(THAL)/MockHalTest.cpp -o $@

run_test_autolink: $(AUTOLINK_SRC) al/AutoLinkTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) al/AutoLinkTest.cpp -o $@

run_test_alink_facade: $(AUTOLINK_SRC) al/AutoLinkFacadeTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) al/AutoLinkFacadeTest.cpp -o $@

run_test_wiresim_closedloop: $(AUTOLINK_SRC) al/WireSimClosedLoopTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) al/WireSimClosedLoopTest.cpp -o $@

# : deterministic clock-injection tests. Pins the three
# time-dependent protocol paths (idle timeout, ACK RTO, sweep stall)
# as sub-ms host tests via MockHal::pumpClock.
run_test_clock_injection: $(AUTOLINK_SRC) al/ClockInjectionTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) al/ClockInjectionTest.cpp -o $@

run_test_alink_io: $(LINK_SRC) $(TPROTO)/LinkIOTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/LinkIOTest.cpp -o $@

run_test_alink_message_roundtrip: $(AUTOLINK_SRC) $(TPROTO)/LinkMessageRoundtripTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/LinkMessageRoundtripTest.cpp -o $@

run_test_alink_message_corrupt: $(AUTOLINK_SRC) $(TPROTO)/LinkMessageCorruptTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/LinkMessageCorruptTest.cpp -o $@

run_test_alink_message_resync: $(AUTOLINK_SRC) $(TPROTO)/LinkMessageResyncTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/LinkMessageResyncTest.cpp -o $@

run_test_alink_message_edge: $(AUTOLINK_SRC) $(TPROTO)/LinkMessageEdgeTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/LinkMessageEdgeTest.cpp -o $@

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
	@echo "=== Building coverage manifest from TEST_BINS ==="
	@mkdir -p coverage
	@printf '%s\n' $(TEST_BINS) > coverage/test_bins.list
	@python3 ../scripts/coverage/coverage_manifest.py Makefile coverage/test_bins.list coverage/manifest.sh
	@echo "=== Merging .gcda counts from every binary ==="
	@../scripts/coverage/coverage_merge.sh coverage/manifest.sh
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

.PHONY: all asan test_asan coverage clean test_cpp dashboard_assets_check \
        test_crc test_cobs test_blink test_framerx test_baudsweep test_log \
        test_mockhal test_alink_io test_alink_message \
        test_alink_error test_alink_cobsseq test_autolink \
        test_alink_web test_alink_baud_preference \
        test_alink_sweep_phase test_alink_sweep_p1_guard \
        test_wiresim_closedloop \
        test_esp_idf_error_etiquette \
        test_alink_arq test_alink_facade \
        test_clock_injection test_linkdecision test_linkreorder \
        test_compile_check test_arq_cache test_sync_mode \
        test_ping_resume_source test_pingpong_structure \
        test_pingpong_log_hygiene \
        test_accessor_structure test_version_free_source \
        test_linkcontext \
        test_httpd_startup \
        test_httpd_reset_endpoint \
        test_coverage_manifest \
        test_boundary_invariants

# Self-test for the coverage manifest generator. Pins AGENTS.md
# rule 4: coverage_merge.sh must not drift from TEST_BINS. The
# generator is the single source of truth — a new suite added to
# TEST_BINS must show up in every src_for_<basename> entry it
# links, with no edit to coverage_merge.sh required.
#
# Both live in ../scripts/coverage/ so the generator, merger, and
# its self-test stay co-located regardless of the suite layout.
test_coverage_manifest:
	@python3 ../scripts/coverage/test_coverage_manifest.py

test_alink_web: run_test_alink_web
	./run_test_alink_web

test_alink_baud_preference: run_test_alink_baud_preference
	./run_test_alink_baud_preference

test_alink_sweep_phase: run_test_alink_sweep_phase
	./run_test_alink_sweep_phase

test_alink_sweep_p1_guard: run_test_alink_sweep_p1_guard
	./run_test_alink_sweep_p1_guard

test_alink_arq: run_test_alink_arq
	./run_test_alink_arq

test_alink_facade: run_test_alink_facade
	./run_test_alink_facade

test_wiresim_closedloop: run_test_wiresim_closedloop
	./run_test_wiresim_closedloop

test_clock_injection: run_test_clock_injection
	./run_test_clock_injection

test_linkdecision: run_test_linkdecision
	./run_test_linkdecision

test_linkreorder: run_test_linkreorder
	./run_test_linkreorder

test_esp_idf_error_etiquette: run_test_esp_idf_error_etiquette
	./run_test_esp_idf_error_etiquette

test_compile_check: run_test_compile_check
	./run_test_compile_check

test_arq_cache: run_test_arq_cache
	./run_test_arq_cache

test_sync_mode: run_test_sync_mode
	./run_test_sync_mode

test_ping_resume_source: run_test_ping_resume_source
	./run_test_ping_resume_source

test_pingpong_structure: run_test_pingpong_structure
	./run_test_pingpong_structure

test_pingpong_log_hygiene: run_test_pingpong_log_hygiene
	./run_test_pingpong_log_hygiene

test_accessor_structure: run_test_accessor_structure
	./run_test_accessor_structure

test_version_free_source: run_test_version_free_source
	./run_test_version_free_source

test_linkcontext: run_test_linkcontext
	./run_test_linkcontext

test_uri_handler_alignment: run_test_uri_handler_alignment
	./run_test_uri_handler_alignment

test_handle_root_chunked: run_test_handle_root_chunked
	./run_test_handle_root_chunked

test_web_begin_lifecycle: run_test_web_begin_lifecycle
	./run_test_web_begin_lifecycle

test_web_httpd_retry: run_test_web_httpd_retry
	./run_test_web_httpd_retry

test_link_begin_defer: run_test_link_begin_defer
	./run_test_link_begin_defer

test_httpd_startup: run_test_httpd_startup
	./run_test_httpd_startup

test_httpd_reset_endpoint: run_test_httpd_reset_endpoint
	./run_test_httpd_reset_endpoint

# Source-level pin for the three small-begin() /
# health-gate fixes: EspHal retries uart_driver_install
# once on ESP_FAIL after a 10 ms vTaskDelay,
# AutoLinkConfig::txBufferSize defaults to 256, and
# bringUpLink halts on !comm.isHealthy() after begin().
# Source-only because EspHal.cpp and PingPongBase.h are
# `#ifdef ARDUINO`.
run_test_esphal_begin_and_health: $(THAL)/EspHalBeginAndHealthTest.cpp
	$(CXX) $(CXXFLAGS) $(THAL)/EspHalBeginAndHealthTest.cpp -o $@

# Source-level pin for the SWP phase single-source
# refactor: PHASE3_ACKS_NEEDED lives in LinkContext.h,
# roundTripMs() is the single round-trip formula in
# LinkDecision.h, clampBuffers() is the AutoLink ctor
# helper, handleSwp_unlocked uses isLockPayload(),
# PromoteToPhase2 is a first-class SwpPhaseAction.
run_test_swp_phase_single_source: $(TSWEEP)/SwpPhaseSingleSourceTest.cpp
	$(CXX) $(CXXFLAGS) $(TSWEEP)/SwpPhaseSingleSourceTest.cpp -o $@

run_test_pong_phase2_entry: $(TSWEEP)/PongPhase2EntryTest.cpp
	$(CXX) $(CXXFLAGS) $(TSWEEP)/PongPhase2EntryTest.cpp -o $@

# Source-level pin for the 5.3.98 P2 dwell-floor
# change: computeDwells writes a flat 250 ms to both
# phase2[i] and phase2Slave[i], no per-baud roundTripMs,
# no < predicate. LinkSweep.cpp is `#ifdef ARDUINO`-free
# but the dwell values are only consumed in the
# sweep-state machine, so the pin reads the source.
run_test_phase2_dwell_floor: $(TSWEEP)/Phase2DwellFloorTest.cpp
	$(CXX) $(CXXFLAGS) $(TSWEEP)/Phase2DwellFloorTest.cpp -o $@

# Runtime + structural pin for the mutual-reset cascade fix:
# pong's P1 guard timer must outlast a single master P2 dwell.
# Master phase2[0] = 250 * N * 1.1 ms; pong initial P1 arm =
# master phase2[0] + 200 ms. Tying the guard to phase2[0] (not
# a magic constant) means it scales with baud count.
run_test_pong_p1_guard_outlasts_master_p2: $(TSWEEP)/PongP1GuardOutlastsMasterP2Test.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) $(TSWEEP)/PongP1GuardOutlastsMasterP2Test.cpp $(LINK_SRC) -o $@

# Source-level pin for the 5.3.98 SYNC/ASYNC mode
# toggle UI + NVS-persist + reboot path.
# AutoLinkWeb.cpp / AutoLinkWebHtml.h / PingPongBase.h
# are all `#ifdef ARDUINO` and can't run on host; this
# gates by reading the source.
run_test_mode_toggle_ui: $(TWEB)/ModeToggleUITest.cpp
	$(CXX) $(CXXFLAGS) $(TWEB)/ModeToggleUITest.cpp -o $@

# Runtime + source-level pin for the SWP-livelock
# BREAK guard: onBreak() in non-OK states is a
# no-op (mirror of err_unlocked), the slave's
# PromoteToPhase2 keeps the P1 baud instead of
# jumping to baud[0], and EspHal.h's break
# debounce window outlasts the UART event-task
# loop period. All four fixes from the livelock
# patch are pinned here.
run_test_onbreak_guard: $(TSWEEP)/OnBreakGuardTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) $(TSWEEP)/OnBreakGuardTest.cpp $(LINK_SRC) -o $@

# Six source-grep pins for the boundary
# invariants fixed in 5.3.102 (TX drain,
# ILinkEvents split, WINDOW ownership,
# baud-count clamp, test-flag eviction,
# named-constant static_assert). No link
# step — pure source audit. Toggle any of
# the six fixes off and the matching pin
# flips red.
run_test_boundary_invariants: al/BoundaryInvariantsTest.cpp
	$(CXX) $(CXXFLAGS) al/BoundaryInvariantsTest.cpp -o $@

test_esphal_begin_and_health: run_test_esphal_begin_and_health
	./run_test_esphal_begin_and_health

test_swp_phase_single_source: run_test_swp_phase_single_source
	./run_test_swp_phase_single_source

test_pong_phase2_entry: run_test_pong_phase2_entry
	./run_test_pong_phase2_entry

test_phase2_dwell_floor: run_test_phase2_dwell_floor
	./run_test_phase2_dwell_floor

test_pong_p1_guard_outlasts_master_p2: run_test_pong_p1_guard_outlasts_master_p2
	./run_test_pong_p1_guard_outlasts_master_p2

test_mode_toggle_ui: run_test_mode_toggle_ui
	./run_test_mode_toggle_ui

test_onbreak_guard: run_test_onbreak_guard
	./run_test_onbreak_guard

test_boundary_invariants: run_test_boundary_invariants
	./run_test_boundary_invariants

# ---- dashboard JS tests (Node + jsdom) ---------------------------------
# Verifies the embedded dashboard JavaScript (poll(), bindLvlGroup(),
# role-conditional UI, etc) without an ESP32. Requires Node + jsdom.
#
# The split refactor carved the original single-file
# dashboard-js-test.js (1383 lines) into:
#   - dashboard-test-harness.js (mock fetch, jsonResp,
#     recordFetch, assert / eq / truthy, setup)
#   - dashboard-role.test.js (role-conditional UI)
#   - dashboard-poll.test.js (poll cycle)
#   - dashboard-log.test.js (log / msg-pause / copy / save / reset)
#   - dashboard-timeout.test.js (fetch timeout contract)
#   - dashboard-js-index.js (thin runner that spawns each spec)
#
# This target is OPT-IN. The C++ test gate (`make`, `make test_cpp`)
# does NOT depend on it. To bootstrap jsdom, run once with network:
#   make test_dashboard_js_bootstrap
# Then any subsequent `make test_dashboard_js` runs the JS test
# (skipping cleanly if node or jsdom is still unavailable). This
# keeps `make` green/red a function of the C++ suite, not of npm
# reachability.
JS_INDEX = dashboard-js-index.js

test_dashboard_js:
	@if [ ! -x "$$(command -v node)" ]; then \
	    echo "  SKIP: node not in PATH"; \
	elif [ ! -d node_modules/jsdom ]; then \
	    echo "  SKIP: jsdom not installed (run 'make test_dashboard_js_bootstrap' once with network)"; \
	else \
	    node $(JS_INDEX); \
	fi

test_dashboard_js_role:
	@if [ ! -x "$$(command -v node)" ]; then \
	    echo "  SKIP: node not in PATH"; \
	elif [ ! -d node_modules/jsdom ]; then \
	    echo "  SKIP: jsdom not installed"; \
	else \
	    node dashboard-role.test.js; \
	fi

test_dashboard_js_poll:
	@if [ ! -x "$$(command -v node)" ]; then \
	    echo "  SKIP: node not in PATH"; \
	elif [ ! -d node_modules/jsdom ]; then \
	    echo "  SKIP: jsdom not installed"; \
	else \
	    node dashboard-poll.test.js; \
	fi

test_dashboard_js_log:
	@if [ ! -x "$$(command -v node)" ]; then \
	    echo "  SKIP: node not in PATH"; \
	elif [ ! -d node_modules/jsdom ]; then \
	    echo "  SKIP: jsdom not installed"; \
	else \
	    node dashboard-log.test.js; \
	fi

test_dashboard_js_timeout:
	@if [ ! -x "$$(command -v node)" ]; then \
	    echo "  SKIP: node not in PATH"; \
	elif [ ! -d node_modules/jsdom ]; then \
	    echo "  SKIP: jsdom not installed"; \
	else \
	    node dashboard-timeout.test.js; \
	fi

# One-time bootstrap. Only invoke this when you have network
# access — `make` and `make test_cpp` do NOT depend on it.
test_dashboard_js_bootstrap: package.json
	@npm install
	@touch node_modules/jsdom

package.json:
	@echo '{ "name": "autolink-test", "version": "0.0.0", "private": true, "dependencies": { "jsdom": "*" } }' > $@

.PHONY: test_dashboard_js test_dashboard_js_role test_dashboard_js_poll test_dashboard_js_log test_dashboard_js_timeout test_dashboard_js_bootstrap
