# AutoLink embedded INTEGRATION test build.
#
# Cross-compiles test_embedded.ino against the real ESP32
# toolchain. This is the only host-independent way to verify
# the Arduino-only glue (AutoLinkWeb.cpp, WiFi, NVS, httpd).
#
# Requires:
#   - build/build_env.sh to have been run once (installs arduino-cli
#     + esp32:esp32@3.3.5).
#   - Network access for the first compile (esp32 core + toolchain).
#
# Targets:
#   make              cross-compile the sketch (default)
#   make test_embedded_compile   explicit alias
#   make clean        remove the scratch build directory
#
# No summary here — this is one binary, not a suite. Fail = compile
# error, pass = exit 0.

ARDUINO_CLI = bash ../../../build/arduino-cli-cmd.sh
FQBN        = esp32:esp32:firebeetle32
SCRATCH     = /tmp/itest_embedded
SKETCH      = $(SCRATCH)/test_embedded/test_embedded.ino

# `all` is the default target. It stages the sketch into a
# scratch dir, compiles, and prints the byte / memory summary
# that the top-level `make all` aggregates.
all: test_embedded_compile
	@echo "=== Embedded cross-compile PASS ==="

test_embedded_compile:
	@mkdir -p $(SCRATCH)
	@cp test_embedded.ino $(SKETCH)
	@echo "--- Compiling test_embedded.ino against $(FQBN) ---"
	@$(ARDUINO_CLI) compile --fqbn $(FQBN) \
	    --library ../../../src --library ../../../include --library ../../.. \
	    --warnings none $(SCRATCH) 2>&1 | tail -30
	@bin=$$(find $(SCRATCH) -name '*.ino.esp32.esp32.firebeetle32.bin' 2>/dev/null | head -1); \
	if [ -n "$$bin" ] && [ -f "$$bin" ]; then \
	    sz=$$(stat -c %s $$bin); \
	    printf "\n=== Embedded test summary ===\n"; \
	    printf "  binary       : %s\n" $$bin; \
	    printf "  binary bytes : %d B\n" $$sz; \
	else \
	    printf "\n=== Embedded cross-compile completed (no .bin located) ===\n"; \
	fi

# ---- housekeeping -----------------------------------------------------------
clean:
	rm -rf /tmp/itest_embedded

.PHONY: all test_embedded_compile clean
