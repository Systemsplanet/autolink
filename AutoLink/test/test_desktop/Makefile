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
CXXFLAGS = -std=c++14 -Wall -Wextra -DAUTOLINK_HOST_TEST -I../../src -I../../src/util
SRC      = ../../src
UTIL     = ../../src/util
LINK_SRC = $(SRC)/ALink.cpp $(SRC)/Log.cpp $(UTIL)/UtilCrc.cpp $(UTIL)/UtilCobs.cpp $(UTIL)/UtilFrameRx.cpp $(UTIL)/UtilBaudSweep.cpp

# Each test binary: <name>Test.cpp + the protocol sources it needs.
# The utility-only tests (Crc/Cobs/Blink/FrameRx/BaudSweep) link just their
# own .cpp; the protocol tests (Log/MockHal/ALink/AutoLink) link the full set.
TEST_BINS = run_test_crc run_test_cobs run_test_blink run_test_framerx \
            run_test_baudsweep run_test_log run_test_mockhal run_test_autolink \
            run_test_alink_io run_test_alink_message run_test_alink_negotiation \
            run_test_alink_error run_test_alink_watchdog

all: $(TEST_BINS)
	@echo "=== All $(words $(TEST_BINS)) test suites PASS ==="

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

test_alink_message: run_test_alink_message
	./run_test_alink_message

test_alink_negotiation: run_test_alink_negotiation
	./run_test_alink_negotiation

test_alink_error: run_test_alink_error
	./run_test_alink_error

test_alink_watchdog: run_test_alink_watchdog
	./run_test_alink_watchdog

run_test_crc: $(UTIL)/UtilCrc.cpp UtilCrcTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilCrc.cpp UtilCrcTest.cpp -o $@

run_test_cobs: $(UTIL)/UtilCobs.cpp UtilCobsTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilCobs.cpp UtilCobsTest.cpp -o $@

run_test_blink: $(UTIL)/UtilBlink.h UtilBlinkTest.cpp
	$(CXX) $(CXXFLAGS) UtilBlinkTest.cpp -o $@

run_test_framerx: $(UTIL)/UtilFrameRx.cpp UtilFrameRxTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilFrameRx.cpp $(UTIL)/UtilCobs.cpp $(UTIL)/UtilCrc.cpp UtilFrameRxTest.cpp -o $@

run_test_baudsweep: $(UTIL)/UtilBaudSweep.cpp UtilBaudSweepTest.cpp
	$(CXX) $(CXXFLAGS) $(UTIL)/UtilBaudSweep.cpp UtilBaudSweepTest.cpp -o $@

run_test_log: $(SRC)/Log.cpp LogTest.cpp
	$(CXX) $(CXXFLAGS) $(SRC)/Log.cpp LogTest.cpp -o $@

run_test_mockhal: $(LINK_SRC) MockHalTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) MockHalTest.cpp -o $@

run_test_autolink: $(LINK_SRC) AutoLinkTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) AutoLinkTest.cpp -o $@

run_test_alink_io: $(LINK_SRC) ALinkIOTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) ALinkIOTest.cpp -o $@

run_test_alink_message: $(LINK_SRC) ALinkMessageTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) ALinkMessageTest.cpp -o $@

run_test_alink_negotiation: $(LINK_SRC) ALinkNegotiationTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) ALinkNegotiationTest.cpp -o $@

run_test_alink_error: $(LINK_SRC) ALinkErrorTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) ALinkErrorTest.cpp -o $@

run_test_alink_watchdog: $(LINK_SRC) ALinkWatchdogTest.cpp
	$(CXX) $(CXXFLAGS) $(LINK_SRC) ALinkWatchdogTest.cpp -o $@

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
        test_alink_error test_alink_watchdog test_autolink
