# AutoLink desktop (host) test build.
#
# Three build modes:
#
#   make                build and run every suite (fast, default)
#   make test_asan      build every suite with AddressSanitizer + UBSan
#                       (catches leaks, use-after-free, undefined behaviour)
#   make coverage       build every suite with --coverage instrumentation,
#                       run them, then produce coverage/coverage.txt via gcov
#
# Per-suite targets (e.g. `make test_crc`) build and run one suite.
#
# Sources live in ../../src (core) and ../../src/util (utilities); both are
# on the include path so headers resolve by the same names the firmware uses.

CXX      = g++
CXXFLAGS = -std=c++14 -Wall -Wextra -DAUTOLINK_HOST_TEST -I. -I../../src -I../../src/al
SRC      = ../../src
AL       = ../../src/al
PROTO    = $(AL)/protocol
HAL      = $(AL)/hal
UTIL     = $(AL)/util
WEB      = $(AL)/web
PONG     = $(AL)/pingpong
# Mirror the src/al/ layout in test/test_desktop/al/<sub>/
TAL      = al
TPROTO   = $(TAL)/protocol
TUTIL    = $(TAL)/util
TWEB     = $(TAL)/web
LINK_SRC = $(PROTO)/ALink.cpp $(PROTO)/UtilBaudSweep.cpp $(PROTO)/UtilFrameRx.cpp $(UTIL)/Log.cpp $(UTIL)/UtilCrc.cpp $(UTIL)/UtilCobs.cpp
AUTOLINK_SRC = $(SRC)/AutoLink.cpp $(LINK_SRC)

# Each test binary: <name>Test.cpp + the protocol sources it needs.
# The utility-only tests (Crc/Cobs/Blink/FrameRx/BaudSweep) link just their
# own .cpp; the protocol tests (Log/MockHal/ALink/AutoLink) link the full set.
TEST_BINS = run_test_crc run_test_cobs run_test_blink run_test_framerx \
            run_test_baudsweep run_test_log run_test_mockhal run_test_autolink \
            run_test_alink_io run_test_alink_message run_test_alink_negotiation \
            run_test_alink_error run_test_alink_watchdog run_test_alink_cobsseq \
            run_test_alink_arq run_test_alink_web run_loopback_noise

LOOPBACK_BIN = run_loopback

# Run the loopback as a 10-second smoke test as part of `all`. The
# binary exits 0 on success, non-zero on failure. Output is terse so
# the all-target log stays readable.
LOOPBACK_QUIET_SECS = 10

all: $(TEST_BINS) $(LOOPBACK_BIN) test_dashboard_js
	@echo "=== $(words $(TEST_BINS)) C++ suites + dashboard JS test PASS ==="
	@./run_loopback_noise
	@echo "=== Running loopback smoke test ($(LOOPBACK_QUIET_SECS)s) ... ==="
	@./run_loopback $(LOOPBACK_QUIET_SECS) 2>&1 | grep -E '^\(PASS\|PARTIAL\|FAIL\):|^Total:|^Ping state:|^Messages TX:|^Pong state:|^T=  +[0-9]+s' | head -25
	@./run_loopback $(LOOPBACK_QUIET_SECS) >/dev/null 2>&1 && echo '=== Loopback PASS ===' || echo '=== Loopback FAIL ==='

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

test_alink_negotiation: run_test_alink_negotiation
	./run_test_alink_negotiation

test_alink_error: run_test_alink_error
	./run_test_alink_error

test_alink_watchdog: run_test_alink_watchdog
	./run_test_alink_watchdog

test_alink_cobsseq: run_test_alink_cobsseq
	./run_test_alink_cobsseq

run_test_alink_cobsseq: $(LINK_SRC) $(TPROTO)/ALinkCobsSeqTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/ALinkCobsSeqTest.cpp -o $@

run_test_alink_arq: $(LINK_SRC) $(TPROTO)/ALinkArqTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/ALinkArqTest.cpp -o $@

run_test_alink_web: $(LINK_SRC) $(WEB)/AutoLinkWebCore.cpp $(TWEB)/AutoLinkWebTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(WEB)/AutoLinkWebCore.cpp $(TWEB)/AutoLinkWebTest.cpp -o $@

run_test_crc: $(UTIL)/UtilCrc.cpp $(TUTIL)/UtilCrcTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilCrc.cpp $(TUTIL)/UtilCrcTest.cpp -o $@

run_test_cobs: $(UTIL)/UtilCobs.cpp $(TUTIL)/UtilCobsTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilCobs.cpp $(TUTIL)/UtilCobsTest.cpp -o $@

run_test_blink: $(UTIL)/UtilBlink.h $(TUTIL)/UtilBlinkTest.cpp
	$(CXX) $(CXXFLAGS) $(TUTIL)/UtilBlinkTest.cpp -o $@

run_test_framerx: $(PROTO)/UtilFrameRx.cpp $(TPROTO)/UtilFrameRxTest.cpp
	$(CXX) $(CXXFLAGS) $(PROTO)/UtilFrameRx.cpp $(UTIL)/UtilCobs.cpp $(UTIL)/UtilCrc.cpp $(TPROTO)/UtilFrameRxTest.cpp -o $@

run_test_baudsweep: $(PROTO)/UtilBaudSweep.cpp $(TPROTO)/UtilBaudSweepTest.cpp
	$(CXX) $(CXXFLAGS) $(PROTO)/UtilBaudSweep.cpp $(TPROTO)/UtilBaudSweepTest.cpp -o $@

run_test_log: $(UTIL)/Log.cpp $(TUTIL)/LogTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/Log.cpp $(TUTIL)/LogTest.cpp -o $@

run_test_mockhal: $(LINK_SRC) MockHalTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) MockHalTest.cpp -o $@

run_test_autolink: $(AUTOLINK_SRC) AutoLinkTest.cpp
	$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) AutoLinkTest.cpp -o $@

run_test_alink_io: $(LINK_SRC) $(TPROTO)/ALinkIOTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/ALinkIOTest.cpp -o $@

run_loopback: $(LINK_SRC) $(TPROTO)/loopback_test.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/loopback_test.cpp -o $@ -pthread

run_loopback_noise: $(LINK_SRC) $(TPROTO)/loopback_noise_test.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/loopback_noise_test.cpp -o $@ -pthread

loopback: run_loopback
	./run_loopback 30

loopback_noise: run_loopback_noise
	./run_loopback_noise

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

run_test_alink_message: $(LINK_SRC) $(TPROTO)/ALinkMessageTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/ALinkMessageTest.cpp -o $@

run_test_alink_negotiation: $(LINK_SRC) $(TPROTO)/ALinkNegotiationTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/ALinkNegotiationTest.cpp -o $@

run_test_alink_error: $(LINK_SRC) $(TPROTO)/ALinkErrorTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/ALinkErrorTest.cpp -o $@

run_test_alink_watchdog: $(LINK_SRC) $(TPROTO)/ALinkWatchdogTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) $(TPROTO)/ALinkWatchdogTest.cpp -o $@

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

.PHONY: all asan test_asan coverage clean \
        test_crc test_cobs test_blink test_framerx test_baudsweep test_log \
        test_mockhal test_alink_io test_alink_message test_alink_negotiation \
        test_alink_error test_alink_watchdog test_alink_cobsseq test_autolink \
        test_alink_web

test_alink_web: run_test_alink_web
	./run_test_alink_web

test_loopback_suite: test_loopback

# ---- dashboard JS test (Node + jsdom) ------------------------------------
# Verifies the embedded dashboard JavaScript (poll(), bindLvlGroup(),
# role-conditional UI, etc) without an ESP32. Requires Node + jsdom:
#   npm install jsdom
# Run: `make test_dashboard_js`
JS_TEST = dashboard-js-test.js
JS_NODE_MODULES = node_modules/jsdom

test_dashboard_js: $(JS_NODE_MODULES)
	node $(JS_TEST)

$(JS_NODE_MODULES): package.json
	npm install
	@touch $@

package.json:
	@echo '{ "name": "autolink-test", "version": "0.0.0", "private": true, "dependencies": { "jsdom": "*" } }' > $@

.PHONY: test_dashboard_js
