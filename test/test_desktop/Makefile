# AutoLink desktop UNIT test build.
#
# Unit tests only — small, subsecond, no wall-clock sleeps, no
# multi-second runs. Two-Link loopback / noise tests live under
# test/itest/test_desktop/ because they take seconds of wall time
# and exercise the protocol end-to-end across two instances.
#
# Build modes:
#
#  make      build and run every suite (default; must stay subsecond)
#  make test_asan build every suite with AddressSanitizer + UBSan
#  make coverage  build every suite with --coverage instrumentation
#
# Per-suite targets (e.g. `make test_crc`) build and run one suite.
#
# Sources live in ../../src (core) and ../../src/util (utilities); both are
# on the include path so headers resolve by the same names the firmware uses.
# Shared test infrastructure (MockHal, WireSim) lives in ../common.
# Test-side scripts (peak_rss, coverage, env) live in ../scripts/{common,coverage,env}.

CXX   = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -DAUTOLINK_HOST_TEST \
      -I. -I../common -I../common/accessors -I../../src -I../../src/al
SRC   = ../../src
AL    = ../../src/al
PROTO  = $(AL)/link
HAL   = $(AL)/hal
UTIL   = $(AL)/util
WEB   = $(AL)/web
PONG   = $(AL)/pingpong
# Mirror the src/al/ layout in test/test_desktop/al/<sub>/
TAL   = al
THAL   = $(TAL)/hal
TPROTO  = $(TAL)/link
TARQ   = $(TPROTO)/arq
TSWEEP  = $(TPROTO)/sweep
TUTIL  = $(TAL)/util
TWEB   = $(TAL)/web
TPING  = $(TAL)/pingpong
LINK_SRC = $(PROTO)/LinkCore.cpp $(PROTO)/LinkApi.cpp \
      $(PROTO)/io/LinkTx.cpp $(PROTO)/io/LinkRx.cpp \
      $(PROTO)/io/LinkFrameRx.cpp $(PROTO)/io/LinkMsgCodec.cpp \
      $(PROTO)/timers/LinkSweepGlue.cpp \
      $(PROTO)/timers/LinkTimersOk.cpp $(PROTO)/timers/LinkTimerBreak.cpp $(PROTO)/timers/LinkTimersSwp.cpp $(PROTO)/timers/LinkBreak.cpp \
      $(PROTO)/arq/ArqCache.cpp $(PROTO)/arq/LinkArq.cpp \
      $(PROTO)/sweep/LinkBaudSweep.cpp $(PROTO)/sweep/LinkSweep.cpp \
      $(UTIL)/Log.cpp $(UTIL)/UtilCrc.cpp $(UTIL)/UtilCobs.cpp
AUTOLINK_SRC = $(SRC)/AutoLink.cpp $(LINK_SRC)

# Unit-test binaries only. Loopback / loopback_noise live under
# test/itest/test_desktop/ (see ../itest/test_desktop/Makefile).
# Regenerates the seven src/al/web/generated/*.h files from
# src/al/web/assets/ before building. --check-only variant used in
# CI; see assets_check target.
dashboard_assets_regen:
	@cd ../.. && python3 build/dashboard_assets.py

assets_check:
	@cd ../.. && python3 build/dashboard_assets.py --check || \
	  (echo 'src/al/web/generated/*.h is stale — run: python3 build/dashboard_assets.py' && exit 1)

TEST_BINS = run_test_crc run_test_cobs run_test_blink run_test_framerx \
      run_test_baudsweep run_test_log run_test_log_queue_drain run_test_log_ring_buffer run_test_mockhal run_test_autolink \
      run_test_alink_io run_test_alink_message_roundtrip \
      run_test_alink_message_corrupt \
      run_test_alink_message_resync \
      run_test_alink_message_edge \
      run_test_alink_error run_test_alink_cobsseq \
      run_test_alink_arq run_test_alink_web \
      run_test_alink_facade run_test_wiresim_closedloop \
      run_test_fill_byte_roundtrip \
      run_test_autolink_begin_order \
      run_test_clock_injection run_test_linkdecision \
      run_test_alink_baud_preference \
      run_test_alink_sweep_phase \
      run_test_alink_sweep_p1_guard \
      run_test_esp_idf_error_etiquette \
      run_test_compile_check \
      run_test_arq_cache \
      run_test_sync_mode \
      run_test_ping_resume_source \
      run_test_pingpong_structure \
      run_test_pingpong_log_hygiene \
      run_test_production_log_level \
      run_test_pingpong_echo_log_level \
      run_test_dropped_count_cross_check \
      run_test_bytes_recvd_forwarded_to_ping \
      run_test_accessor_structure \
      run_test_version_free_source \
      run_test_subsystem_logging \
      run_test_p2_slave_walk_all_bauds \
      run_test_p3_slave_camp_outlasts_master \
      run_test_break_baud_aware_confirm \
      run_test_break_storm_forces_p1_walk \
      run_test_break_storm_defers_to_on_timer \
      run_test_break_fast_confirm_after_frames \
      run_test_baud_aware_stuck_threshold_per_tick \
      run_test_rate_limit_rollover_check \
      run_test_linkcontext \
      run_test_uri_handler_alignment \
      run_test_handle_root_chunked \
      run_test_web_begin_lifecycle \
      run_test_web_httpd_retry \
      run_test_link_begin_defer \
      run_test_httpd_startup \
      run_test_httpd_reset_endpoint \
      run_test_esphal_begin_and_health \
      run_test_esphal_stream_buf_abort \
      run_test_esphal_stream_buf_floor \
      run_test_heap_cap_floor \
      run_test_ota_core \
      run_test_pong_phase2_entry \
      run_test_phase2_dwell_floor \
      run_test_sweep_timeout_arms \
      run_test_pong_p1_guard_outlasts_master_p2 \
      run_test_mode_toggle_ui \
      run_test_onbreak_guard \
      run_test_kickoff_enters_p1_once \
      run_test_kickoff_gates_on_state \
      run_test_peer_resync_on_missed_break \
      run_test_epoch_bounce \
      run_test_p3_bail_watchdog \
      run_test_break_confirm \
      run_test_disc_storm_escalation \
      run_test_boundary_invariants \
      run_test_ping_send_failure \
      run_test_ack_bytes \
      run_test_rx_bytes_wire_ack \
      run_test_baud_index_bounds \
      run_test_ping_gap_transition \
      run_test_app_buf_full_admit_nothing \
      run_test_wiresim_app_buf_full \
      run_test_settle_gate \
      run_test_stats_include_app_state \
      run_test_seq_space_guard \
      run_test_async_random_admission \
      run_test_pingpong_blink_and_delay \
      run_test_ping_mismatch_count \
      run_test_mtu_roundtrip \
      run_test_mode_sync_async_fixes \
      run_test_base_seq_tracking \
      run_test_tx_stall_watchdog \
      run_test_fast_retx \
      run_test_sync_stall_watchdog \
      run_test_dead_link_watchdog \
      run_test_field_wedge_fixes \
      run_test_health \
      run_test_timer_rearm \
      run_test_ok_timer_always_arms \
      run_test_msg_codec \
      run_test_timer_phases \
      run_test_sync_resync_spiral \
      run_test_linkrx_split_ctrl \
      run_test_gbn_backoff \
      run_test_async_chunk_gap \
      run_test_gbn_burst_cap \
      run_test_gbn_drop_policy \
      run_test_gbn_keep_rearm \
      run_test_ok_keepalive \
      run_test_reset_reason_diag \
      run_test_break_interrupt_coalesce \
      run_test_link_health_baud_aware \
      run_test_peer_baud_mismatch \
      run_test_wiresim_re_converge \
      run_test_resync_scan_err \
      run_test_last_valid_rx_ms \
      run_test_send_msg_reason_enum \

# 'all' runs every unit suite, captures per-suite runtime, then
# prints a summary: tests passed, total bytes used by the suite
# binaries (proxy for resident memory on disk), and peak RSS as
# observed via /usr/bin/time. Unit tests should remain subsecond
# end-to-end; the per-suite wall time is printed so any regression
# that pushes a suite over the budget is visible immediately.
all: dashboard_assets_regen $(TEST_BINS)
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
	    printf " [PASS] %-40s %5d ms %7d B rss=%s KiB\n" $$t $$ms $$sz $$rss; \
	    pass=$$(( pass + 1 )); \
	  else \
	    printf " [FAIL] %-40s %5d ms\n" $$t $$ms; \
	    fail=$$(( fail + 1 )); \
	    fail_names="$$fail_names $$t"; \
	  fi; \
	done; \
	end_ns=$$(date +%s%N); \
	total_ms=$$(( (end_ns - start_ns) / 1000000 )); \
	printf "\n=== Unit test summary ===\n"; \
	printf " tests passed : %d / %d\n" $$pass $(words $(TEST_BINS)); \
	if [ $$fail -gt 0 ]; then printf " FAILED    :%s\n" "$$fail_names"; fi; \
	printf " total time  : %d ms\n" $$total_ms; \
	printf " total bytes : %d B (sum of suite binaries on disk)\n" $$total_bytes; \
	printf " peak RSS   : %d KiB (largest single-suite resident set)\n" $$peak_kb; \
	if [ $$fail -gt 0 ]; then exit 1; fi; \
	if [ $$total_ms -gt 60000 ]; then \
	  printf " WARNING   : unit suite exceeded 60 s wall budget — see rule in AGENTS.md\n"; \
	fi; \
	printf "=== Unit test suite PASS ===\n"

# C++-only gate: build + run every suite. Use this when you
# need a green/red signal independent of dashboard JS.
test_cpp: $(TEST_BINS)
	@fail=0; \
	for t in $(TEST_BINS); do \
	  echo " $$t"; \
	  ./$$t >/dev/null 2>&1 && echo "  PASS" || { echo "  FAIL"; fail=1; }; \
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

run_test_alink_cobsseq: $(LINK_SRC) $(TPROTO)/frame/LinkCobsSeqTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/frame/LinkCobsSeqTest.cpp -o $@

run_test_alink_arq: $(LINK_SRC) $(TARQ)/LinkArqTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TARQ)/LinkArqTest.cpp -o $@

run_test_linkdecision: $(TSWEEP)/LinkDecisionTest.cpp
	$(CXX) $(CXXFLAGS) $(TSWEEP)/LinkDecisionTest.cpp -o $@

run_test_alink_web: $(LINK_SRC) $(WEB)/AutoLinkWebCore.cpp $(TWEB)/AutoLinkWebTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(WEB)/AutoLinkWebCore.cpp $(TWEB)/AutoLinkWebTest.cpp -o $@

run_test_alink_baud_preference: $(LINK_SRC) $(TSWEEP)/LinkBaudPreferenceTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TSWEEP)/LinkBaudPreferenceTest.cpp -o $@

run_test_alink_sweep_phase: $(LINK_SRC) $(TSWEEP)/guard/LinkSweepPhaseTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TSWEEP)/guard/LinkSweepPhaseTest.cpp -o $@

run_test_alink_sweep_p1_guard: $(LINK_SRC) $(TSWEEP)/guard/LinkSweepP1GuardTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TSWEEP)/guard/LinkSweepP1GuardTest.cpp -o $@


# Static-analysis test for ESP-IDF error-etiquette — reads
# the .cpp/.h source files and asserts every known ESP-IDF
# call site has a paired esp_err_to_name log.
run_test_esp_idf_error_etiquette: al/meta/EspIdfErrorEtiquetteTest.cpp
	$(CXX) $(CXXFLAGS) al/meta/EspIdfErrorEtiquetteTest.cpp -o $@

run_test_compile_check: al/meta/CompileCheckTest.cpp
	$(CXX) $(CXXFLAGS) al/meta/CompileCheckTest.cpp -o $@

run_test_arq_cache: $(AUTOLINK_SRC) al/link/arq/ArqCacheTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) al/link/arq/ArqCacheTest.cpp -o $@

run_test_sync_mode: $(AUTOLINK_SRC) al/facade/SyncModeTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) al/facade/SyncModeTest.cpp -o $@

# Source-level regression test for Ping::setPaused resume
# contract. Ping is #ifdef ARDUINO so we can't
# construct one on host; we pin the proactive clearQueue_()
# call by reading the source.
run_test_ping_resume_source: $(TPING)/ping/PingResumeSourceTest.cpp
	$(CXX) $(CXXFLAGS) $(TPING)/ping/PingResumeSourceTest.cpp -o $@

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

# Production log-level pin: initSerial in
# PingPongBase.h must set the runtime + ESP-IDF
# default to INFO, not DEBUG. The prior DEBUG
# default flooded the log with per-echo chatter
# and would re-flood again on any regression that
# re-raises a per-echo line to debug. Two source-
# grep pins.
run_test_production_log_level: $(TPING)/ProductionLogLevelTest.cpp
	$(CXX) $(CXXFLAGS) $(TPING)/ProductionLogLevelTest.cpp -o $@

# Source-level pins for the per-echo log-level demotion
# in Ping::loop() and Pong::loop(). The per-ACKed-echo
# chatter at debug level floods at the ASYNC pipeline
# rate (MAX_TX_PER_LOOP per loop = hundreds/sec) and
# drowns the very state-transition lines needed to
# diagnose a wedge. Contract: per-echo lines are at
# verbose level.
run_test_pingpong_echo_log_level: $(TPING)/PingPongEchoLogLevelTest.cpp
	$(CXX) $(CXXFLAGS) $(TPING)/PingPongEchoLogLevelTest.cpp -o $@

run_test_dropped_count_cross_check: al/pingpong/DroppedCountCrossCheckTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) al/pingpong/DroppedCountCrossCheckTest.cpp $(LINK_SRC) -o $@

# Source-level pin for the AutoLink facade
# bytesRecvdForMessage forwarder + Ping's
# wire-recvd log line. Closes Open 5 from
# todo.md: the link-layer accessor now has
# a production consumer (the wire-recvd log
# line at every slot completion). Source-
# grep is the right gate because Ping and
# AutoLink are ARDUINO-gated and can't run
# on host.
run_test_bytes_recvd_forwarded_to_ping: $(TPING)/BytesRecvdForwardedToPingTest.cpp
	$(CXX) $(CXXFLAGS) $(TPING)/BytesRecvdForwardedToPingTest.cpp -o $@

# test_* / *_ForTest hooks moved off
# Link / AutoLink public API into
# LinkTestAccessor / AutoLinkTestAccessor
# shims under test/common/. Source-level
# pin: reads Link.h and AutoLink.h, asserts
# the public sections no longer declare
# the hooks and the friend declarations
# for the shims are present.
run_test_accessor_structure: $(LINK_SRC) $(AUTOLINK_SRC) $(TPROTO)/misc/TestAccessorStructureTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/misc/TestAccessorStructureTest.cpp -o $@

run_test_version_free_source: al/meta/VersionFreeSourceTest.cpp
	$(CXX) $(CXXFLAGS) al/meta/VersionFreeSourceTest.cpp -o $@

# Per-subsystem log-instrumentation pin: every
# major subsystem must have at least one
# info-level state-transition log line AND at
# least one debug-level deep-trace line. The
# hot-path subsystems (wire TX, wire RX) are
# also pinned to NOT have any info-level logs
# in their per-frame function bodies, since
# info at the ASYNC pipeline rate would flood
# the log sink. Source-level (no link required).
run_test_subsystem_logging: al/meta/SubsystemLoggingTest.cpp
	$(CXX) $(CXXFLAGS) al/meta/SubsystemLoggingTest.cpp -o $@

# this-release field-log wedge batch — behavioural
# regression tests placed in their concern directories.
# Each pin is toggle-verifiable: red when the fix is
# reverted, green when the fix is in place. Source-grep
# tests cannot catch behavioural regressions that keep
# the source text but break the runtime (e.g. an
# off-by-one in a P2 walk still has the wrap line in
# place), so every change here ships with a runtime
# test on the actual code path.
run_test_p2_slave_walk_all_bauds: al/link/sweep/SlaveP2WalkAllBaudsTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) al/link/sweep/SlaveP2WalkAllBaudsTest.cpp $(LINK_SRC) -o $@

run_test_p3_slave_camp_outlasts_master: al/link/sweep/SlaveP3CampOutlastsMasterTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) al/link/sweep/SlaveP3CampOutlastsMasterTest.cpp $(LINK_SRC) -o $@

run_test_break_baud_aware_confirm: al/link/health/BreakBaudAwareConfirmTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) al/link/health/BreakBaudAwareConfirmTest.cpp $(LINK_SRC) -o $@

run_test_break_storm_forces_p1_walk: al/link/health/BreakStormForcesP1WalkTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) al/link/health/BreakStormForcesP1WalkTest.cpp $(LINK_SRC) -o $@

run_test_break_storm_defers_to_on_timer: al/link/health/BreakStormDefersToOnTimerTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) al/link/health/BreakStormDefersToOnTimerTest.cpp $(LINK_SRC) -o $@

run_test_break_fast_confirm_after_frames: al/link/health/BreakFastConfirmAfterFramesTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) al/link/health/BreakFastConfirmAfterFramesTest.cpp $(LINK_SRC) -o $@

run_test_baud_aware_stuck_threshold_per_tick: al/link/gbn/BaudAwareStuckThresholdPerTickTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) al/link/gbn/BaudAwareStuckThresholdPerTickTest.cpp $(LINK_SRC) -o $@

run_test_rate_limit_rollover_check: al/link/async/RateLimitRolloverCheckTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) al/link/async/RateLimitRolloverCheckTest.cpp $(LINK_SRC) -o $@


# LinkArq / LinkSweep drive the link through
# LinkContext — not friendship. Source-level pin (no friend
# declarations in Link.h, no #include "al/link/Link.h" in any
# helper) plus a runtime mock-context that the helpers compile
# against. Compile-only does most of the work: if a future change
# reverts the helper signatures to Link&, this test stops
# compiling because MockCtx isn't a Link.
run_test_linkcontext: $(PROTO)/LinkCore.cpp $(PROTO)/io/LinkTx.cpp $(PROTO)/io/LinkRx.cpp \
           $(PROTO)/timers/LinkSweepGlue.cpp \
           $(PROTO)/timers/LinkTimersOk.cpp $(PROTO)/timers/LinkTimerBreak.cpp $(PROTO)/timers/LinkTimersSwp.cpp $(PROTO)/timers/LinkBreak.cpp \
           $(PROTO)/LinkApi.cpp \
           $(PROTO)/io/LinkFrameRx.cpp $(PROTO)/io/LinkMsgCodec.cpp \
           $(PROTO)/arq/LinkArq.cpp $(PROTO)/sweep/LinkBaudSweep.cpp $(PROTO)/sweep/LinkSweep.cpp \
           $(UTIL)/Log.cpp $(UTIL)/UtilCrc.cpp $(UTIL)/UtilCobs.cpp \
           $(TPROTO)/misc/LinkContextTest.cpp
	$(CXX) $(CXXFLAGS) $(PROTO)/LinkCore.cpp $(PROTO)/io/LinkTx.cpp $(PROTO)/io/LinkRx.cpp \
		$(PROTO)/timers/LinkSweepGlue.cpp \
		$(PROTO)/timers/LinkTimersOk.cpp $(PROTO)/timers/LinkTimerBreak.cpp $(PROTO)/timers/LinkTimersSwp.cpp $(PROTO)/timers/LinkBreak.cpp \
		$(PROTO)/LinkApi.cpp \
		$(PROTO)/io/LinkFrameRx.cpp $(PROTO)/io/LinkMsgCodec.cpp \
		$(PROTO)/arq/LinkArq.cpp $(PROTO)/sweep/LinkBaudSweep.cpp $(PROTO)/sweep/LinkSweep.cpp \
		$(UTIL)/Log.cpp $(UTIL)/UtilCrc.cpp $(UTIL)/UtilCobs.cpp \
		$(TPROTO)/misc/LinkContextTest.cpp -o $@

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
# contract. AutoLinkWeb's httpd handlers are `#ifdef ARDUINO` and can't run
# on host; this gates the bug by reading the source. Covers
# the silent-truncation failure mode where the ~28 KB
# dashboard HTML is shipped in a single `httpd_resp_send`
# call against a 4096-byte httpd send buffer and the
# browser receives a malformed / empty page.
run_test_handle_root_chunked: $(TWEB)/HandleRootChunkedTest.cpp
	$(CXX) $(CXXFLAGS) $(TWEB)/HandleRootChunkedTest.cpp -o $@

run_test_web_begin_lifecycle: $(TWEB)/httpd/WebBeginLifecycleTest.cpp
	$(CXX) $(CXXFLAGS) $(TWEB)/httpd/WebBeginLifecycleTest.cpp -o $@

run_test_web_httpd_retry: $(TWEB)/httpd/WebHttpdRetryTest.cpp
	$(CXX) $(CXXFLAGS) $(TWEB)/httpd/WebHttpdRetryTest.cpp -o $@

run_test_link_begin_defer: $(TWEB)/httpd/LinkBeginDeferTest.cpp
	$(CXX) $(CXXFLAGS) $(TWEB)/httpd/LinkBeginDeferTest.cpp -o $@

# Source-level pin for the httpd startup race and the
# TIME_WAIT retry budget. AutoLinkWeb's httpd handlers are `#ifdef ARDUINO`
# so it can't run on host; this gates the bug by reading
# the source. Covers:
#  - Bug 1: double-entry race between begin() and
#   wifiTaskThunk_() on setupHttpAndLogging_().
#  - Bug 2: HTTPD_RETRY_MAX * HTTPD_RETRY_PRE_MS must
#   cover lwIP TCP TIME_WAIT (~60 s).
#  - Bug 3: uart_driver_install ESP_FAIL recovery —
#   EspHal::begin() must pre-clear with
#   uart_is_driver_installed + uart_driver_delete.
run_test_httpd_startup: $(TWEB)/httpd/HttpdStartupTest.cpp
	$(CXX) $(CXXFLAGS) $(TWEB)/httpd/HttpdStartupTest.cpp -o $@

# End-to-end HTTP test for the /reset endpoint. Stands up
# a loopback TCP server in a thread that mirrors
# AutoLinkWeb::handleReset()'s contract (resetStats +
# resetErrors + resetDiag + emit "ok" with text/plain),
# then drives a real HTTP POST /reset via the system
# curl. Verifies the body equals "ok" and the handler
# is path-scoped (POST /garbage does NOT fire reset).
# Caps total wall time at 5 s via curl --max-time and
# pthread_join timeout, per AGENTS rule 4.
run_test_httpd_reset_endpoint: $(AUTOLINK_SRC) $(TWEB)/httpd/HttpdResetEndpointTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TWEB)/httpd/HttpdResetEndpointTest.cpp -o $@ -pthread

run_test_crc: $(UTIL)/UtilCrc.cpp $(TUTIL)/UtilCrcTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilCrc.cpp $(TUTIL)/UtilCrcTest.cpp -o $@

run_test_cobs: $(UTIL)/UtilCobs.cpp $(TUTIL)/UtilCobsTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilCobs.cpp $(TUTIL)/UtilCobsTest.cpp -o $@

run_test_blink: $(UTIL)/UtilBlink.h $(TUTIL)/UtilBlinkTest.cpp
	$(CXX) $(CXXFLAGS) $(TUTIL)/UtilBlinkTest.cpp -o $@

run_test_framerx: $(PROTO)/io/LinkFrameRx.cpp $(PROTO)/io/LinkMsgCodec.cpp $(TPROTO)/frame/LinkFrameRxTest.cpp
	$(CXX) $(CXXFLAGS) $(PROTO)/io/LinkFrameRx.cpp $(PROTO)/io/LinkMsgCodec.cpp $(UTIL)/UtilCobs.cpp $(UTIL)/UtilCrc.cpp $(TPROTO)/frame/LinkFrameRxTest.cpp -o $@

run_test_baudsweep: $(PROTO)/sweep/LinkBaudSweep.cpp $(TSWEEP)/LinkBaudSweepTest.cpp
	$(CXX) $(CXXFLAGS) $(PROTO)/sweep/LinkBaudSweep.cpp $(TSWEEP)/LinkBaudSweepTest.cpp -o $@

run_test_log: $(UTIL)/Log.cpp $(TUTIL)/LogTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/Log.cpp $(TUTIL)/LogTest.cpp -o $@

# Queue/drain regression pin for the host-side Log
# transport. Field log surfaced dropped state-transition
# lines right after a reset_unlocked reason line, matching
# fflush(stdout) blocking on a slow UART. The fix routes
# the host stdout write through a bounded queue +
# drainPending() so emit() is non-blocking. Five pins:
# emit() does not call fflush inline, emit() enqueues
# into pending_, drainPending() flushes the queue, queue
# overflow drops the OLDEST line, and emit() is
# non-blocking on overflow.
run_test_log_queue_drain: $(UTIL)/Log.cpp $(TUTIL)/LogQueueDrainTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/Log.cpp $(TUTIL)/LogQueueDrainTest.cpp -o $@

# Header-only ring buffer core (no locking, no RTOS dependency) —
# pinned in isolation so the wraparound/drop-oldest/capacity
# invariants are verified on host before al/util/Log.cpp wraps it
# with a FreeRTOS critical section for the ESP32 background log
# drain task (see docs/Version.md for why: ESP_LOGx was never
# actually verified non-blocking on-device, and the field's
# multi-second stalls right after a verbose-logging burst point
# straight at it).
run_test_log_ring_buffer: $(TUTIL)/LogRingBufferTest.cpp
	$(CXX) $(CXXFLAGS) $(TUTIL)/LogRingBufferTest.cpp -o $@

run_test_mockhal: $(LINK_SRC) $(THAL)/MockHalTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(THAL)/MockHalTest.cpp -o $@

run_test_autolink: $(AUTOLINK_SRC) al/facade/AutoLinkTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) al/facade/AutoLinkTest.cpp -o $@

run_test_alink_facade: $(AUTOLINK_SRC) al/facade/AutoLinkFacadeTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) al/facade/AutoLinkFacadeTest.cpp -o $@

run_test_wiresim_closedloop: $(AUTOLINK_SRC) al/facade/WireSimClosedLoopTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) al/facade/WireSimClosedLoopTest.cpp -o $@

run_test_fill_byte_roundtrip: $(AUTOLINK_SRC) al/facade/FillByteRoundtripTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) al/facade/FillByteRoundtripTest.cpp -o $@

run_test_autolink_begin_order: al/facade/AutoLinkBeginOrderTest.cpp
	$(CXX) $(CXXFLAGS) al/facade/AutoLinkBeginOrderTest.cpp -o $@

# : deterministic clock-injection tests. Pins the three
# time-dependent protocol paths (idle timeout, ACK RTO, sweep stall)
# as sub-ms host tests via MockHal::pumpClock.
run_test_clock_injection: $(AUTOLINK_SRC) al/facade/ClockInjectionTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) al/facade/ClockInjectionTest.cpp -o $@

run_test_alink_io: $(LINK_SRC) $(TPROTO)/misc/LinkIOTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/misc/LinkIOTest.cpp -o $@

run_test_alink_message_roundtrip: $(AUTOLINK_SRC) $(TPROTO)/message/LinkMessageRoundtripTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/message/LinkMessageRoundtripTest.cpp -o $@

run_test_alink_message_corrupt: $(AUTOLINK_SRC) $(TPROTO)/message/LinkMessageCorruptTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/message/LinkMessageCorruptTest.cpp -o $@

run_test_alink_message_resync: $(AUTOLINK_SRC) $(TPROTO)/message/LinkMessageResyncTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/message/LinkMessageResyncTest.cpp -o $@

run_test_alink_message_edge: $(AUTOLINK_SRC) $(TPROTO)/message/LinkMessageEdgeTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/message/LinkMessageEdgeTest.cpp -o $@

run_test_alink_error: $(LINK_SRC) $(TPROTO)/health/LinkErrorTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/health/LinkErrorTest.cpp -o $@

# ---- integration: AddressSanitizer + UBSan ----------------------------------
# Catches the same categories valgrind would catch (out-of-bounds, use-after-
# free, leaks) but is gcc-native and orders of magnitude faster. Run after
# `make clean` to drop the non-instrumented binaries.
ASAN_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1
ASAN_LIBS  = -fsanitize=address,undefined

asan: CXXFLAGS += $(ASAN_FLAGS)
asan: LDLIBS  = $(ASAN_LIBS)
asan: clean all

test_asan: asan
	@echo "=== ASan + UBSan run complete (no diagnostics = pass) ==="

# ---- integration: gcov coverage --------------------------------------------
# Builds with --coverage, runs every suite (so .gcda files are written),
# then runs gcov on each .gcno file to produce annotated coverage.
COV_FLAGS = -fprofile-arcs -ftest-coverage -fbranch-probabilities -O0
COV_LIBS = -lgcov

coverage: CXXFLAGS += $(COV_FLAGS)
coverage: LDLIBS  = $(COV_LIBS)
coverage: clean
	@$(MAKE) all CXXFLAGS="$(CXXFLAGS)" LDLIBS="$(LDLIBS)"
	@echo "=== Running every test suite to gather .gcda files ==="
	@for t in $(TEST_BINS); do \
	  echo " $$t"; \
	  ./$$t > /dev/null 2>&1 || { echo " $$t FAILED"; exit 1; }; \
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
	      if (tag == "-") next; # non-executable line, skip \
	      lines++; \
	      if (tag+0 > 0) hit++; \
	    } \
	    /^branch[[:space:]]+[0-9]+/ { \
	      btot++; \
	      if ($$0 !~ /never[[:space:]]+executed/) bhit++; \
	    } \
	    END { printf " %-40s %5d/%-5d lines (%5.1f%%) %3d/%-3d branches\n", \
	      f, hit, lines, lines?hit*100.0/lines:0, bhit, btot }' $$g; \
	done | sort -k4 -nr | tee -a coverage/coverage.txt
	@echo "=== Coverage report in coverage/coverage.txt ==="

# ---- housekeeping -----------------------------------------------------------
clean:
	rm -f $(TEST_BINS) *.gcno *.gcda
	rm -rf coverage

.PHONY: all asan test_asan coverage clean test_cpp dashboard_assets_regen assets_check \
    test_crc test_cobs test_blink test_framerx test_baudsweep test_log \
    test_mockhal test_alink_io test_alink_message \
    test_alink_error test_alink_cobsseq test_autolink \
    test_alink_web test_alink_baud_preference \
    test_alink_sweep_phase test_alink_sweep_p1_guard \
    test_wiresim_closedloop \
    test_esp_idf_error_etiquette \
    test_alink_arq test_alink_facade \
    test_clock_injection test_linkdecision \
    test_compile_check test_arq_cache test_sync_mode \
    test_ping_resume_source test_pingpong_structure \
    test_pingpong_log_hygiene \
    test_accessor_structure test_version_free_source \
    test_p2_slave_walk_all_bauds \
    test_p3_slave_camp_outlasts_master \
    test_break_baud_aware_confirm \
    test_break_storm_forces_p1_walk \
    test_break_storm_defers_to_on_timer \
    test_break_fast_confirm_after_frames \
    test_baud_aware_stuck_threshold_per_tick \
    test_rate_limit_rollover_check \
    test_linkcontext \
    test_httpd_startup \
    test_httpd_reset_endpoint \
    test_coverage_manifest \
    test_boundary_invariants \
    test_gbn_backoff test_async_chunk_gap test_gbn_burst_cap

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

test_bytes_recvd_forwarded_to_ping: run_test_bytes_recvd_forwarded_to_ping
	./run_test_bytes_recvd_forwarded_to_ping

test_accessor_structure: run_test_accessor_structure
	./run_test_accessor_structure

test_version_free_source: run_test_version_free_source
	./run_test_version_free_source

test_subsystem_logging: run_test_subsystem_logging
	./run_test_subsystem_logging

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

# Source-level pin for the EspHal::begin() heap-
# refusal abort path: xStreamBufferCreate failure
# must clean up mutex + task_exit_sem, set
# running=false, and return without setting
# healthy=true. EspHal.h is the source-of-truth
# (no .cpp), so the pin reads it directly.
run_test_esphal_stream_buf_abort: $(THAL)/EspHalStreamBufAbortTest.cpp
	$(CXX) $(CXXFLAGS) $(THAL)/EspHalStreamBufAbortTest.cpp -o $@

# Source + runtime pin for the stream-buffer
# floor: streamBufferFloor(cfg) at the default
# maxMsg must stay under a heap-realistic 32 KB
# ceiling and must be >= maxMsg. Pins the formula location
# (AutoLinkConfig.h) and the thin forward in
# EspHal.h; pins that LinkCore.cpp drops the
# bogus maxMsg > streamBufferSize check.
run_test_esphal_stream_buf_floor: $(THAL)/EspHalStreamBufFloorTest.cpp
	$(CXX) $(CXXFLAGS) $(THAL)/EspHalStreamBufFloorTest.cpp -o $@

run_test_heap_cap_floor: $(THAL)/HeapCapFloorTest.cpp
	$(CXX) $(CXXFLAGS) $(THAL)/HeapCapFloorTest.cpp -o $@

run_test_ota_core: $(WEB)/OtaCore.cpp $(TWEB)/OtaCoreTest.cpp
	$(CXX) $(CXXFLAGS) $(WEB)/OtaCore.cpp $(UTIL)/Log.cpp $(TWEB)/OtaCoreTest.cpp -o $@

run_test_pong_phase2_entry: $(TSWEEP)/guard/PongPhase2EntryTest.cpp
	$(CXX) $(CXXFLAGS) $(TSWEEP)/guard/PongPhase2EntryTest.cpp -o $@

# Source-level pin for the P2 dwell-floor
# change: computeDwells writes a flat 250 ms to both
# phase2[i] and phase2Slave[i], no per-baud roundTripMs,
# no < predicate. LinkSweep.cpp is `#ifdef ARDUINO`-free
# but the dwell values are only consumed in the
# sweep-state machine, so the pin reads the source.
run_test_phase2_dwell_floor: $(TSWEEP)/Phase2DwellFloorTest.cpp
	$(CXX) $(CXXFLAGS) $(TSWEEP)/Phase2DwellFloorTest.cpp -o $@

# Clock-injected host pin for the sweep-timeout arms
# in LinkTimers.cpp::onTimerSwp_unlocked (master P3 +
# pong P2). Both decision helpers
# (decideMasterPhase3Timeout / decidePongPhase2Timeout)
# are pure-function-tested in LinkDecisionTest; this
# suite pins the *Link-side* arm so a future refactor
# that drops the decision call is caught by the
# runtime pin (toggle-off would let the dwell expire
# silently into a no-op).
run_test_sweep_timeout_arms: $(LINK_SRC) $(TSWEEP)/SweepTimeoutArmsTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TSWEEP)/SweepTimeoutArmsTest.cpp -o $@

# Runtime + structural pin for the mutual-reset cascade fix:
# pong's P1 guard timer must outlast a single master P2 dwell.
# Master phase2[0] = 250 * N * 1.1 ms; pong initial P1 arm =
# master phase2[0] + 200 ms. Tying the guard to phase2[0] (not
# a magic constant) means it scales with baud count.
run_test_pong_p1_guard_outlasts_master_p2: $(TSWEEP)/guard/PongP1GuardOutlastsMasterP2Test.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) $(TSWEEP)/guard/PongP1GuardOutlastsMasterP2Test.cpp $(LINK_SRC) -o $@

# Source-level pin for the SYNC/ASYNC mode
# toggle UI + NVS-persist + reboot path.
# AutoLinkWeb's httpd handlers / src/al/web/generated/*.h / PingPongBase.h
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
run_test_onbreak_guard: $(TSWEEP)/guard/OnBreakGuardTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) $(TSWEEP)/guard/OnBreakGuardTest.cpp $(LINK_SRC) -o $@

run_test_kickoff_enters_p1_once: $(TSWEEP)/guard/KickoffEntersPhase1OnceTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) $(TSWEEP)/guard/KickoffEntersPhase1OnceTest.cpp $(LINK_SRC) -o $@

# kickoff() must gate on link state, not on a permanent
# kickedOff_ latch — a kickedOff_ latch is a one-shot
# and turns every recovery kickoff after the first
# wedge into a silent no-op, with no path short of
# reboot. Four pins: no-op in OK, re-fires master
# start in SWP, recovers from a wedge, source-grep on
# the state guard.
run_test_kickoff_gates_on_state: $(TSWEEP)/guard/KickoffGatesOnStateTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) $(TSWEEP)/guard/KickoffGatesOnStateTest.cpp $(LINK_SRC) -o $@

# One-sided reset (no BREAK delivered to the still-OK peer)
# resyncs via the sweep-frame seq-byte session epoch. Pinned
# by 3 axes: the production bug shape (Pin 1, runtime),
# the load-bearing source shape in processCtrlFrame_unlocked
# (Pin 2, source-grep), and the lockstep epoch bump in
# reset_unlocked (Pin 3, source-grep).
run_test_peer_resync_on_missed_break: $(TSWEEP)/guard/PeerResyncOnMissedBreakTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) $(TSWEEP)/guard/PeerResyncOnMissedBreakTest.cpp $(LINK_SRC) -o $@

# Fix for the field-log epoch slide (peer 0->1->2->3->4 on both
# sides, never converging): a PeerEpochMismatch-triggered reset
# must not bump our own sweepEpoch_, or two correctly-functioning
# detectors hand one glitch back and forth forever. Pin 1 (unit),
# Pin 2 (source-grep), Pin 3 (runtime, two-node, the real storm).
run_test_epoch_bounce: $(TSWEEP)/guard/EpochBounceTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) $(TSWEEP)/guard/EpochBounceTest.cpp $(LINK_SRC) -o $@

# P3 / resweepPrefPending_ bail path re-arms a watchdog
# so a concurrent state/phase/guard trip doesn't leave
# the link with no armed timer (a bare return would
# mean onTimer never fires again, link dead). Three
# runtime pins (timer armed, armed on phase change,
# re-armed timer actually fires) + one source-grep pin
# on the load-bearing hw.startTimer(phase1ArmMs()) call.
run_test_p3_bail_watchdog: $(TSWEEP)/guard/P3BailWatchdogTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) $(TSWEEP)/guard/P3BailWatchdogTest.cpp $(LINK_SRC) -o $@

# Six source-grep pins for the boundary
# invariants fixed in the TX-drain + pull-from-appBuf batch (TX drain,
# ILinkEvents split, WINDOW ownership,
# baud-count clamp, test-flag eviction,
# named-constant static_assert). No link
# step — pure source audit. Toggle any of
# the six fixes off and the matching pin
# flips red.
run_test_boundary_invariants: al/meta/BoundaryInvariantsTest.cpp
	$(CXX) $(CXXFLAGS) al/meta/BoundaryInvariantsTest.cpp -o $@

# Source-level + structural pins for the
# this-release Ping/Pong role changes:
# Pong-no-echo, Ping "echo <seq> <bytes> <pending>"
# log, consecSendFail_ escalation, msg-size policy
# (sequential + random), gap-stop / gap-resume,
# got<0 drain. Pin set covers the Ping/Pong
# surface without constructing either role on host
# (both are `#ifdef ARDUINO`).
run_test_ping_send_failure: $(TPING)/ping/PingSendFailureTest.cpp
	$(CXX) $(CXXFLAGS) $(TPING)/ping/PingSendFailureTest.cpp -o $@

# Runtime + structural pins for the extended 5-byte wire ACK frame
# (carries bytes-recvd in addition to the seq). Covers end-to-end
# round-trip + pool-exhaustion drop + HoldAck-NAK paths.
run_test_ack_bytes: $(LINK_SRC) $(TPROTO)/ack/LinkAckBytesTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/ack/LinkAckBytesTest.cpp -o $@

# Runtime + structural pin for the inbound wire ACK/NAK byte
# counter: it must count bytes on the wire-ACK path too, not only
# the app-payload path, or a ping whose peer only sends wire ACKs
# (no echo) sees rxBytes stuck at 0 forever. This suite pins the
# symmetric inbound count.
run_test_rx_bytes_wire_ack: $(LINK_SRC) $(TPROTO)/ack/RxBytesWireAckTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/ack/RxBytesWireAckTest.cpp -o $@

# Runtime pin for the post-construction OOB-closed
# shape: cfg.allowedBaudsCount = 20 (or -1) after
# link.begin() must not drive spdI into an OOB read.
# The choke-point accessors in Link clamp the count
# on the way out; this suite exercises the bound.
run_test_baud_index_bounds: $(LINK_SRC) $(TPROTO)/misc/LinkBaudIndexBoundsTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/misc/LinkBaudIndexBoundsTest.cpp -o $@

# Runtime + table test for the gap-stop transition
# extracted from Ping::loop into PingGap.h. Exercises
# the entry edge (NO_GAP -> gap-stop on a NAK), the
# update path (new NAK while paused), the resume
# (ACK of gap seq), and the predates-gap edge case
# (lastAck == lastNak at entry). Plus a source-grep
# pin that catches a regression where the
# unconditional read moves back inside the
# if (gapSeq_ != NO_GAP) block.
run_test_ping_gap_transition: $(LINK_SRC) $(TPING)/ping/PingGapTransitionTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPING)/ping/PingGapTransitionTest.cpp -o $@

test_ping_gap_transition: run_test_ping_gap_transition
	./run_test_ping_gap_transition

test_app_buf_full_admit_nothing: run_test_app_buf_full_admit_nothing
	./run_test_app_buf_full_admit_nothing

test_settle_gate: run_test_settle_gate
	./run_test_settle_gate

test_stats_include_app_state: run_test_stats_include_app_state
	./run_test_stats_include_app_state

# NAK-loss fix: app-buf-full NAK must
# write nothing and not advance rxSeq. Source-
# grep only (no link required). The bug class
# was a partial pushAppBuf that spliced
# garbage into the message stream, plus a
# pre-NAK rxSeq advance that made the retx
# classify as Stale and re-ACK away.
run_test_app_buf_full_admit_nothing: $(TPROTO)/message/AppBufFullAdmitNothingTest.cpp
	$(CXX) $(CXXFLAGS) $(TPROTO)/message/AppBufFullAdmitNothingTest.cpp -o $@

# WireSim end-to-end counterpart to
# run_test_app_buf_full_admit_nothing. Drives
# the full state machine (pipe + COBS + ARQ
# + app-buf drain + message reassembler)
# with a constrained receiver app buf. Asserts
# no frameErrs on the wire (a partial write
# would generate CRC/desync rejections from
# the message parser) and that the link
# resumes data flow after a NAK storm.
run_test_wiresim_app_buf_full: $(AUTOLINK_SRC) $(TPROTO)/message/WireSimAppBufFullTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/message/WireSimAppBufFullTest.cpp -o $@

# settle-gate fix: lockOk_unlocked
# drains the app buf + UART FIFO and opens a
# settle window during which every wire path
# The settle window gates on validation, not on
# arrival: a CRC-valid frame inside the window is
# processed, CRC-failed garbage is rejected without
# counting a link fault. Behavioural — links the
# real sources against MockHal.
run_test_settle_gate: $(AUTOLINK_SRC) $(TPROTO)/timer/SettleGateTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/timer/SettleGateTest.cpp -o $@

# stats-always-on fix: logStats line
# always carries the app state (gapStopped,
# gapMissing, paused, lastSendMsgReason), and
# Ping::loop's gap-stop early-return path
# calls logStats with gapStopped=true so a
# wedge self-identifies in the next 5s tick.
# Source-grep only.
run_test_stats_include_app_state: $(TPING)/StatsIncludeAppStateTest.cpp
	$(CXX) $(CXXFLAGS) $(TPING)/StatsIncludeAppStateTest.cpp -o $@

# Runtime + source-grep pins for the seq-space
# exhaustion guard in LinkApi.cpp and the budget-
# vs-msg static_asserts in AutoLinkConfig.h. Drives
# arq_.pendingCount() to a near-full value and
# asserts sendMsg() rejects; reads the budget /
# maxMsg / MAX_CHUNK invariants from the source.
run_test_seq_space_guard: $(LINK_SRC) $(TPROTO)/frame/LinkSeqSpaceGuardTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/frame/LinkSeqSpaceGuardTest.cpp -o $@

# ASYNC-random death-spiral pin. A full-maxMsg random draw (22
# chunks) cannot co-admit with a loaded GBN window; sendMsg rejects
# it, the app cools down, and the stuck base spins whole-window
# retransmits until the link drops. Ping's RANDOM draw is capped to
# maxLenForChunkBudget(window/2) so every random message stays
# co-admittable. Drives the real ArqCache window and source-pins
# the ceiling constant.
run_test_async_random_admission: $(LINK_SRC) $(TPROTO)/async/AsyncRandomAdmissionTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/async/AsyncRandomAdmissionTest.cpp -o $@

# Source-level pins for the ping-blink-on-send,
# pong-blink-on-recv, and HTML GUI default delay-ms
# (50 ms) fixes. Tiny shape locks for the dashboard
# + Ping/Pong loop bodies; runtime path is already
# exercised by WireSimClosedLoopTest's blink
# accounting.
run_test_pingpong_blink_and_delay: $(TPING)/PingPongBlinkAndDelayTest.cpp
	$(CXX) $(CXXFLAGS) $(TPING)/PingPongBlinkAndDelayTest.cpp -o $@

# Source-level pin for the Ping mismatchCount field: the field must
# actually be incremented, not just declared and passed through to
# logStats(). The test pins (a) the got<0 branch in Ping::loop
# bumps mismatchCount_++ AFTER the diag log and
# BEFORE clearQueue_(), and (b) the labeled
# companion echo log line ("echo#=%llu seq=%u
# msgBytes=%u pending=%d") is present at both ack
# sites with successEchoCount_ as the monotonic
# first field, so the second field reads unambiguously
# as a length counter.
run_test_ping_mismatch_count: $(TPING)/ping/PingMismatchCountTest.cpp
	$(CXX) $(CXXFLAGS) $(TPING)/ping/PingMismatchCountTest.cpp -o $@

# MTU end-to-end round-trip. SYNC and ASYNC modes
# both deliver a 32 KB message byte-for-byte across
# the 133-chunk frame-build + per-chunk ACK + ARQ
# cache eviction path. Pairs with run_test_seq_space_guard
# (which pins the rejection path) to cover both
# halves of the multi-chunk MTU wire contract.
run_test_mtu_roundtrip: $(LINK_SRC) $(TPROTO)/message/LinkMtuRoundtripTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/message/LinkMtuRoundtripTest.cpp -o $@

# Source + runtime regression test for the
# this-release ASYNC link-thrash fixes:
# IHal::setMode + AutoLink::setMode forward
# (Pin 1), uartRxBufferFloor / uartTxBufferFloor
# mode-based scaling (Pin 2), okTickMs ASYNC
# clamp at one RTO (Pin 3), wire-frame
# txBytes counter for CTRL/ACK/NAK/retx
# (Pin 4), bringUpLink version-first log + Ping
# 1000 ms backpressure cooldown (Pin 5). Each
# pin fails when its fix is reverted.
run_test_mode_sync_async_fixes: $(LINK_SRC) $(TPROTO)/async/ModeSyncAsyncFixesTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/async/ModeSyncAsyncFixesTest.cpp -o $@

run_test_sync_resync_spiral: $(LINK_SRC) $(TPROTO)/timer/SyncResyncSpiralTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/timer/SyncResyncSpiralTest.cpp -o $@

# Split OK-state CTRL frame regression: a 5-byte 0xAA 0x55 CTRL
# frame delivered across two onRx() chunks must be held (okCarry_)
# and reassembled, not dropped into the COBS framer.
run_test_linkrx_split_ctrl: $(LINK_SRC) $(TPROTO)/frame/LinkRxSplitCtrlTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/frame/LinkRxSplitCtrlTest.cpp -o $@

# Pins the GBN whole-window retransmit backoff:
# decideGbnBackoff math (noProgress=1 unchanged;
# doubling up to cap), source pin on the
# Link/LinkTimers wire-up, and three runtime scenarios —
# stuck base grows backoff exponentially under cap,
# forward ACK resets gbnAttempts_ + gbnBackoffMs_ to 0,
# honest maxRetx drop still fires under the cap-bounded
# backoff (the cap keeps maxRetx reachable within a
# bounded wall budget).
run_test_gbn_backoff: $(LINK_SRC) $(TPROTO)/gbn/GbnBackoffTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/gbn/GbnBackoffTest.cpp -o $@

# Pins the ASYNC inter-chunk pacing gap:
# interChunkGapMs_unlocked() returns 0 in SYNC
# mode and cfg.asyncChunkGapMs in ASYNC mode; the public
# IHal::delayUs primitive exists with a default that
# delegates to delayMs and an EspHal override using
# ets_delay_us; a multi-chunk ASYNC sendMsg emits exactly
# (chunks-1) delayUs calls of (cfg.asyncChunkGapMs*1000)
# us each; SYNC mode short-circuits to 0 delayUs calls.
run_test_async_chunk_gap: $(LINK_SRC) $(TPROTO)/async/AsyncChunkGapTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/async/AsyncChunkGapTest.cpp -o $@

# Pins the GBN whole-window retransmit burst cap
# (the ASYNC-random death-spiral fix's per-RTO
# width knob). decideGbnResendCap math (pending<cap,
# ==cap, >cap, disable), source pin on the
# AutoLinkConfig field + LinkTimers wire-up,
# and three runtime scenarios — wide pending window clipped to
# cap, narrow pending window passed through unchanged, cap=0
# disables the resend.
run_test_gbn_burst_cap: $(LINK_SRC) $(TPROTO)/gbn/GbnBurstCapTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/gbn/GbnBurstCapTest.cpp -o $@

# Pins the peer-gone vs reverse-congested drop policy:
# decideGbnDropOnMaxRetx math (silent/keep/rto=0 boundaries), source pin on
# LinkDecision + LinkTimers wire-up, and two runtime
# scenarios — silent base drops honestly, live-ACK base
# keeps + resets the retx counter so the next stall
# starts fresh.
run_test_gbn_drop_policy: $(LINK_SRC) $(TPROTO)/gbn/GbnDropPolicyTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/gbn/GbnDropPolicyTest.cpp -o $@

# Pins the resweep / recovery wedge: keep livelock
# rearm (rearmSlot clears retxCount_ + sentAtMs_ so
# the next RTO actually retransmits), consecutive-Keep
# rescue cap (force-Drop past 3 keeps so a dead peer
# whose floating RX keeps stamping lastRxMs cannot
# ride the keep path forever), forward-progress reset
# (cumulative-ACK handler clears consecutiveKeep_),
# and the master P3 relock-miss enterPhase1
# fallback contract (state stays SWP, phase=PHASE1,
# timer armed, PING on the wire).
run_test_gbn_keep_rearm: $(LINK_SRC) $(TPROTO)/gbn/GbnKeepRearmTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/gbn/GbnKeepRearmTest.cpp -o $@

# Pins the OK-state keepalive (PING_CMD emitted
# from onTimerOk_unlocked every idleTimeoutMs/2).
# Without the keepalive, the silent-peer watchdog
# (DropSilentPeer, 3 * idleTimeoutMs) tears down a
# healthy idle link on duty-cycled deployments.
# Two nodes, OK, no application traffic, step
# 4 * idleTimeoutMs: both must stay State::OK and
# discCount must not increment (Pin 1). Second
# pin (Pin 2) asserts the keepalive does not
# touch the link's ARQ state (structural pin
# against re-introducing an ACK-shaped keepalive).
# Toggle off the keepalive (delete the keepalive
# from onTimerOk_unlocked) -> Pin 1 red with
# discCount >= 1.
run_test_ok_keepalive: $(AUTOLINK_SRC) $(TPROTO)/health/OkKeepaliveTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/health/OkKeepaliveTest.cpp -o $@

# Per-reason reset counter surfaced in getDiag() so a
# future unexplained OK -> SWP transition (debug-flooded
# log sink) is traceable. Plus the unconditional
# "reset_unlocked reason=" log line that fires on every
# reset. Six pins cover four call-site counters
# (ErrThreshold, ErrRate, UserDropLink, HealthWatchdog),
# the monotonic total, and the unconditional-log source
# shape.
run_test_reset_reason_diag: $(LINK_SRC) $(TPROTO)/health/ResetReasonDiagTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/health/ResetReasonDiagTest.cpp -o $@

# Fix for the field-log seed event: a UART_BREAK while OK is
# reported by the same driver path for a genuine peer detach AND
# for a framing glitch under sustained 512000-baud ASYNC traffic
# with large messages. Debounced ONLY when the link is busy
# (Pin 1-3); an idle link resets immediately, same as before
# (Pin 4 — the OkKeepaliveTest regression this pin prevents).
run_test_break_confirm: $(LINK_SRC) $(TPROTO)/health/BreakConfirmTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/health/BreakConfirmTest.cpp -o $@

# : BREAK-interrupt-coalescing pin. A single
# electrical glitch surfaces on the ESP32 UART driver
# as multiple BREAK / framing-error events at sub-ms
# spacing. The the prior release onBreak path took the second
# sub-ms-spaced BREAK as independent corroboration of
# a real detach and fast-confirmed the reset — tearing
# down a healthy link. The fix coalesces
# BREAK interrupts within BREAK_COALESCE_MS into one
# event. Two pins: (1) two sub-ms-spaced BREAKs do
# NOT reset the link; (2) source-grep on the
# BREAK_COALESCE_MS gate inside onBreak().
run_test_break_interrupt_coalesce: $(LINK_SRC) $(TPROTO)/health/BreakInterruptCoalesceTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/health/BreakInterruptCoalesceTest.cpp -o $@

# : baud-aware Link health. DropAsymIdle and
# the post-lock quiet gate were baud-invariant — a
# low-baud re-lock (9600) had a faster time-to-drop
# than the time needed to actually deliver the queued
# payload (field log: 2.7-3.6s drop at 9600 vs the
# 9s baseline). The fix derives h.rtoMs from
# roundTripMs at the locked baud and floors
# txQuiet_unlocked at the baud-aware rtoMs. Four
# source-grep pins on the new signature and helpers.
run_test_link_health_baud_aware: $(TPROTO)/health/LinkHealthBaudAwareTest.cpp $(LINK_SRC)
	$(CXX) $(CXXFLAGS) $(TPROTO)/health/LinkHealthBaudAwareTest.cpp $(LINK_SRC) -o $@

# : peer-baud-mismatch escalation. After
# kPeerBaudMismatchThreshold consecutive successful
# locks each producing zero valid application frames
# before the next reset, applyHealth_unlocked
# escalates to DropPeerBaudMismatch so the next
# reset forces a full P1 walk instead of re-running
# the same top-down sweep that just failed. Four
# pins cover the increment path, the recv-clear path,
# the runtime escalation, and the source-grep
# threshold.
run_test_peer_baud_mismatch: $(LINK_SRC) $(TPROTO)/health/PeerBaudMismatchTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/health/PeerBaudMismatchTest.cpp -o $@

# : re-convergence after HealthWatchdog reset.
# The prior master/slave sweep-phase skew (master
# fast-paths to preserved baud; slave always walks
# P1 slowest) made every post-BREAK resweep lock
# to 9600 in the field log. The fix gives the slave
# the same preserved-baud fast path on a
# HealthWatchdog-reason reset. Two pins: (1) runtime
# re-convergence on the proven baud within a bounded
# number of cycles, (2) source-grep on the slave's
# fast path (no isMaster gate on the fast-relock
# condition).
run_test_wiresim_re_converge: $(LINK_SRC) $(TSWEEP)/guard/WireSimReConvergeTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TSWEEP)/guard/WireSimReConvergeTest.cpp -o $@

# : resync-scan err() pin. The prior shape
# only called err() on drop > 0; findMsgHeaderResync_unlocked
# returning 0 or -1 (both genuine errors) bypassed it
# and the errThreshold / errRateWindow paths never
# saw the bad-header case. The new shape is err()
# unconditional. Two pins: (1) runtime: a bad-header
# recvMsg fires err() and returns -1; (2) source-grep
# on the unconditional err() call.
run_test_resync_scan_err: $(LINK_SRC) $(TPROTO)/frame/ResyncScanErrTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/frame/ResyncScanErrTest.cpp -o $@

# : lastValidRxMs pin. lastRxMs is stamped on
# any received byte, including a noise byte that
# happens to land on a 0xAA 0x55 preamble and clear
# CTRL carry state. Keying the health machine off
# lastValidRxMs (CRC-validated frames only)
# distinguishes a link that's actually exchanging
# data from one that's seeing a noise heartbeat.
# Three pins: (1) noise bytes do not stamp
# lastValidRxMs; (2) real data frames do; (3)
# source-grep on the applyHealth_unlocked assign.
run_test_last_valid_rx_ms: $(LINK_SRC) $(TPROTO)/health/LastValidRxMsTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/health/LastValidRxMsTest.cpp -o $@

# : SendMsgReason enum pin. Ping's "send failed
# (backpressure)" log line conflated three distinct
# failure reasons (postLockQuiet, gbnWindowFull,
# poolExhaust, notOk) under one label. The new
# SendMsgReason enum is stamped on every false
# return so the app can log the precise cause.
# Three pins: (1) each failure path stamps the
# correct reason; (2) successful sendMsg stamps
# Ok; (3) source-grep on the enum + facade.
run_test_send_msg_reason_enum: $(LINK_SRC) $(TPROTO)/health/SendMsgReasonEnumTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/health/SendMsgReasonEnumTest.cpp -o $@

# Fix for the fast-disconnect storm found alongside the epoch
# bounce: after DISC_STORM_THRESHOLD fast disconnects inside the
# 10s window, the "proven" preferredBaud_ isn't proven anymore.
# reset_unlocked must stop fast-relocking back into whatever's
# causing the churn and force a full P1 walk instead.
run_test_disc_storm_escalation: $(LINK_SRC) $(TPROTO)/health/DiscStormEscalationTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/health/DiscStormEscalationTest.cpp -o $@

# Pins the base-seq tracking carry-over (todo
# item 4 of the ARQ-cache pin set):
# bytesRecvdForMessage returns MSG_HDR + payload
# for a 22-chunk ASYNC message, and the baseSeq_
# table is correctly populated for both short
# (single-chunk) and long (multi-chunk) sends.
# Source-grep pins on LinkArq.h (budgetIdx scheme),
# Link.h (bytesRecvdForMessage body), and LinkArq.cpp
# (onSent NO_BASE default). Runtime pin drives
# a 5120-byte ASYNC send and asserts the
# per-message sum equals 5126.
run_test_base_seq_tracking: $(LINK_SRC) $(TPROTO)/ack/LinkBaseSeqTrackingTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/ack/LinkBaseSeqTrackingTest.cpp -o $@

# Runtime pins for the TX-backpressure stall
# watchdog (third watchdog input, LinkTimers.cpp
# onTimerOk). Drives the wedge shape (full ARQ
# pool, zero pending) and asserts the OK-state
# timer drops + resweeps; plus the streak-clear
# and stale-streak negative pins.
run_test_tx_stall_watchdog: $(LINK_SRC) $(TPROTO)/health/LinkTxStallWatchdogTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/health/LinkTxStallWatchdogTest.cpp -o $@

run_test_fast_retx: $(LINK_SRC) $(TPROTO)/async/LinkFastRetxTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/async/LinkFastRetxTest.cpp -o $@

run_test_sync_stall_watchdog: $(LINK_SRC) $(TPROTO)/health/LinkSyncStallWatchdogTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/health/LinkSyncStallWatchdogTest.cpp -o $@ -pthread

run_test_dead_link_watchdog: $(AUTOLINK_SRC) $(TPROTO)/health/LinkDeadLinkWatchdogTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/health/LinkDeadLinkWatchdogTest.cpp -o $@ -pthread

run_test_field_wedge_fixes: $(AUTOLINK_SRC) $(TPROTO)/FieldWedgeFixesTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) $(TPROTO)/FieldWedgeFixesTest.cpp -o $@ -pthread

run_test_health: $(TPROTO)/health/LinkHealthTest.cpp
	$(CXX) $(CXXFLAGS) $(TPROTO)/health/LinkHealthTest.cpp -o $@

run_test_timer_rearm: $(LINK_SRC) $(TPROTO)/timer/LinkTimerRearmTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/timer/LinkTimerRearmTest.cpp -o $@

# fix-1 regression pin: lockOk_unlocked
# must arm the OK-state timer unconditionally,
# even when cfg.idleTimeoutMs == 0. The OK
# timer is the only GBN RTO retx driver in OK
# state; gating it on idleTimeoutMs disables
# retransmission entirely and wedges the GBN
# window on a single lost frame. Pinned by
# the lockOk arms + fires + source-grep pins.
run_test_ok_timer_always_arms: $(LINK_SRC) $(TPROTO)/timer/OkTimerAlwaysArmsTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/timer/OkTimerAlwaysArmsTest.cpp -o $@


run_test_msg_codec: $(PROTO)/io/LinkMsgCodec.cpp $(UTIL)/UtilCrc.cpp $(TPROTO)/frame/LinkMsgCodecTest.cpp
	$(CXX) $(CXXFLAGS) $(PROTO)/io/LinkMsgCodec.cpp $(UTIL)/UtilCrc.cpp $(TPROTO)/frame/LinkMsgCodecTest.cpp -o $@

run_test_timer_phases: $(LINK_SRC) $(TPROTO)/timer/LinkTimerPhasesTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/timer/LinkTimerPhasesTest.cpp -o $@

test_esphal_begin_and_health: run_test_esphal_begin_and_health
	./run_test_esphal_begin_and_health

test_pong_phase2_entry: run_test_pong_phase2_entry
	./run_test_pong_phase2_entry

test_phase2_dwell_floor: run_test_phase2_dwell_floor
	./run_test_phase2_dwell_floor

test_sweep_timeout_arms: run_test_sweep_timeout_arms
	./run_test_sweep_timeout_arms

test_pong_p1_guard_outlasts_master_p2: run_test_pong_p1_guard_outlasts_master_p2
	./run_test_pong_p1_guard_outlasts_master_p2

test_mode_toggle_ui: run_test_mode_toggle_ui
	./run_test_mode_toggle_ui

test_onbreak_guard: run_test_onbreak_guard
	./run_test_onbreak_guard

test_kickoff_enters_p1_once: run_test_kickoff_enters_p1_once
	./run_test_kickoff_enters_p1_once

test_peer_resync_on_missed_break: run_test_peer_resync_on_missed_break
	./run_test_peer_resync_on_missed_break

test_epoch_bounce: run_test_epoch_bounce
	./run_test_epoch_bounce

test_boundary_invariants: run_test_boundary_invariants
	./run_test_boundary_invariants

test_ping_send_failure: run_test_ping_send_failure
	./run_test_ping_send_failure

test_ack_bytes: run_test_ack_bytes
	./run_test_ack_bytes

test_rx_bytes_wire_ack: run_test_rx_bytes_wire_ack
	./run_test_rx_bytes_wire_ack

test_esphal_stream_buf_abort: run_test_esphal_stream_buf_abort
	./run_test_esphal_stream_buf_abort

test_esphal_stream_buf_floor: run_test_esphal_stream_buf_floor
	./run_test_esphal_stream_buf_floor

test_heap_cap_floor: run_test_heap_cap_floor
	./run_test_heap_cap_floor

test_ota_core: run_test_ota_core
	./run_test_ota_core

test_seq_space_guard: run_test_seq_space_guard
	./run_test_seq_space_guard

test_async_random_admission: run_test_async_random_admission
	./run_test_async_random_admission

test_pingpong_blink_and_delay: run_test_pingpong_blink_and_delay
	./run_test_pingpong_blink_and_delay

test_ping_mismatch_count: run_test_ping_mismatch_count
	./run_test_ping_mismatch_count

test_mtu_roundtrip: run_test_mtu_roundtrip
	./run_test_mtu_roundtrip

test_mode_sync_async_fixes: run_test_mode_sync_async_fixes
	./run_test_mode_sync_async_fixes

test_base_seq_tracking: run_test_base_seq_tracking
	./run_test_base_seq_tracking

# ---- dashboard JS tests (Node + jsdom) ---------------------------------
# Verifies the embedded dashboard JavaScript (poll(), bindLvlGroup(),
# role-conditional UI, etc) without an ESP32. Requires Node + jsdom.
#
# The split refactor carved the original single-file
# dashboard-js-test.js (1383 lines) into:
#  - dashboard-test-harness.js (mock fetch, jsonResp,
#   recordFetch, assert / eq / truthy, setup)
#  - dashboard-role.test.js (role-conditional UI)
#  - dashboard-poll.test.js (poll cycle)
#  - dashboard-log.test.js (log / msg-pause / copy / save / reset)
#  - dashboard-timeout.test.js (fetch timeout contract)
#  - dashboard-js-index.js (thin runner that spawns each spec)
#
# This target is OPT-IN. The C++ test gate (`make`, `make test_cpp`)
# does NOT depend on it. To bootstrap jsdom, run once with network:
#  make test_dashboard_js_bootstrap
# Then any subsequent `make test_dashboard_js` runs the JS test
# (skipping cleanly if node or jsdom is still unavailable). This
# keeps `make` green/red a function of the C++ suite, not of npm
# reachability.
JS_INDEX = dashboard-js/dashboard-js-index.js

test_dashboard_js:
	@if [ ! -x "$$(command -v node)" ]; then \
	  echo " SKIP: node not in PATH"; \
	elif [ ! -d node_modules/jsdom ]; then \
	  echo " SKIP: jsdom not installed (run 'make test_dashboard_js_bootstrap' once with network)"; \
	else \
	  node $(JS_INDEX); \
	fi

test_dashboard_js_role:
	@if [ ! -x "$$(command -v node)" ]; then \
	  echo " SKIP: node not in PATH"; \
	elif [ ! -d node_modules/jsdom ]; then \
	  echo " SKIP: jsdom not installed"; \
	else \
	  node dashboard-js/dashboard-role.test.js; \
	fi

test_dashboard_js_poll:
	@if [ ! -x "$$(command -v node)" ]; then \
	  echo " SKIP: node not in PATH"; \
	elif [ ! -d node_modules/jsdom ]; then \
	  echo " SKIP: jsdom not installed"; \
	else \
	  node dashboard-js/dashboard-poll.test.js; \
	fi

test_dashboard_js_log:
	@if [ ! -x "$$(command -v node)" ]; then \
	  echo " SKIP: node not in PATH"; \
	elif [ ! -d node_modules/jsdom ]; then \
	  echo " SKIP: jsdom not installed"; \
	else \
	  node dashboard-js/dashboard-log.test.js; \
	fi

test_dashboard_js_timeout:
	@if [ ! -x "$$(command -v node)" ]; then \
	  echo " SKIP: node not in PATH"; \
	elif [ ! -d node_modules/jsdom ]; then \
	  echo " SKIP: jsdom not installed"; \
	else \
	  node dashboard-js/dashboard-timeout.test.js; \
	fi

# One-time bootstrap. Only invoke this when you have network
# access — `make` and `make test_cpp` do NOT depend on it.
test_dashboard_js_bootstrap: package.json
	@npm install
	@touch node_modules/jsdom

package.json:
	@echo '{ "name": "autolink-test", "version": "0.0.0", "private": true, "dependencies": { "jsdom": "*" } }' > $@

.PHONY: test_dashboard_js test_dashboard_js_role test_dashboard_js_poll test_dashboard_js_log test_dashboard_js_timeout test_dashboard_js_bootstrap
