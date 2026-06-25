# AutoLink test orchestrator.
#
# Layout:
#   test/common/                 shared test infrastructure (MockHal, WireSim,
#                                peak_rss.py, summarize.py)
#   test/test_desktop/           UNIT tests — small, subsecond, no wall-clock
#   test/itest/test_desktop/     host INTEGRATION tests — two-Link loopback
#                                end-to-end, multi-second
#   test/itest/test_embedded/    Arduino-sketch INTEGRATION test —
#                                cross-compiled against the real ESP32 toolchain
#
# Targets:
#   make test    run only unit tests (test/test_desktop/Makefile)
#   make itest   run only integration tests (test/itest/test_desktop/Makefile)
#   make all     run BOTH, then print a combined summary with
#                tests passed, total bytes, peak RSS / memory
#   make clean   clean both subdirs
#
# `make itest` deliberately does NOT cross-compile test_embedded —
# that target needs a working arduino-cli toolchain. Run
#   cd test/itest/test_embedded && make
# explicitly when you want the ESP32 cross-compile gate.

.PHONY: test itest all clean help

help:
	@echo "Targets:"
	@echo "  test    - run unit tests (test/test_desktop/)"
	@echo "  itest   - run host integration tests (test/itest/test_desktop/)"
	@echo "  all     - run both, print combined summary"
	@echo "  clean   - clean both subdirs"
	@echo ""
	@echo "For ESP32 sketch cross-compile:"
	@echo "  cd test/itest/test_embedded && make"

test:
	@$(MAKE) -C test_desktop all

itest:
	@$(MAKE) -C itest/test_desktop all

# 'all' runs the unit + host integration suites in sequence,
# capturing each suite's stdout, then prints a combined summary:
# tests passed, total bytes (sum of every suite binary on disk),
# peak RSS (largest single-suite resident set observed across
# both suites).
all:
	@start_ns=$$(date +%s%N); \
	echo "--- Running unit tests (test/test_desktop/) ---"; \
	$(MAKE) -C test_desktop all 2>&1 | tee /tmp/unit.out; \
	echo "--- Running integration tests (test/itest/test_desktop/) ---"; \
	$(MAKE) -C itest/test_desktop all 2>&1 | tee /tmp/itest.out; \
	end_ns=$$(date +%s%N); \
	total_ms=$$(( (end_ns - start_ns) / 1000000 )); \
	python3 common/summarize.py /tmp/unit.out /tmp/itest.out; \
	rc=$$?; \
	printf "  orchestrator wall: %d ms (build + run + parse)\n" $$total_ms; \
	if [ $$rc -eq 0 ]; then printf "=== ALL tests PASS ===\n"; else printf "=== ALL tests FAIL ===\n"; fi; \
	exit $$rc

clean:
	@$(MAKE) -C test_desktop clean
	@$(MAKE) -C itest/test_desktop clean
