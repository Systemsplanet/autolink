# AutoLink master Makefile.
#
# Single entry point for "did the build pass?" — runs every test
# the project has, on both the host (g++ suites, dashboard JS,
# loopback) and the ESP32 target (cross-compile). Fails fast on
# the first error so CI and pre-zip checks share one command.
#
# Targets (run from project root with `make -f build/Makefile <t>`
# or `cd build && make <t>`):
#
#   make             (default) — same as `make ci`
#   make test        — host-side: all C++ suites + dashboard JS + 10s loopback
#   make verify      — ESP32 cross-compile via build/verify_build.sh
#   make env         — install arduino-cli + esp32 core (idempotent)
#   make ci          — `make test` then `make verify`; fail fast
#   make clean       — remove host test artifacts + verify_build cache
#                      (delegates to build/clean.py; pass CLEAN_FLAGS
#                       for e.g. --apply to actually delete)
#   make loopback    — 30s host loopback smoke
#   make coverage    — host tests with gcov instrumentation
#   make asan        — host tests with AddressSanitizer + UBSan
#
# Sources for the host tests live in test/test_desktop/Makefile;
# this file is just orchestration. Keep it boring.

ROOT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
TEST_DIR := $(ROOT_DIR)/test/test_desktop

.PHONY: all test verify env ci clean loopback coverage asan help

all: ci

help:
	@echo "AutoLink master Makefile"
	@echo "  make test        - host tests (C++ suites + dashboard JS + loopback)"
	@echo "  make verify      - ESP32 cross-compile (build/verify_build.sh)"
	@echo "  make env         - install arduino-cli + esp32 core"
	@echo "  make ci          - test + verify, fail fast (default)"
	@echo "  make clean       - remove host test artifacts + verify_build cache"
	@echo "  make loopback    - 30s host loopback smoke"
	@echo "  make coverage    - host tests with gcov"
	@echo "  make asan        - host tests with AddressSanitizer + UBSan"

test:
	@$(MAKE) -C $(TEST_DIR)

verify:
	@bash $(ROOT_DIR)/build/verify_build.sh

env:
	@bash $(ROOT_DIR)/build/build_env.sh

ci: test verify
	@echo "=== make ci: host tests + ESP32 cross-compile PASS ==="

loopback:
	@$(MAKE) -C $(TEST_DIR) loopback

coverage:
	@$(MAKE) -C $(TEST_DIR) coverage

asan:
	@$(MAKE) -C $(TEST_DIR) test_asan

clean:
	@python3 $(ROOT_DIR)/build/clean.py --apply $(CLEAN_FLAGS)
	@$(MAKE) -C $(TEST_DIR) clean
	@echo "=== make clean: host artifacts + ESP32 build cache removed ==="
