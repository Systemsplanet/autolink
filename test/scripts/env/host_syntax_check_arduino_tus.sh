#!/usr/bin/env bash
# AL-A3: 8 of 28 .cpp files in src/ are ARDUINO-only and were never
# compiled by any host test — EspHal.cpp, Link.cpp,
# AutoLinkWebCore.cpp, OtaCore.cpp, and the four src/al/web/handlers/
# TUs. 1,907 lines shipped in every release, 12% of source, with zero
# host-side type/syntax checking. Every test file that touched one of
# them said "cannot compile on host" in a comment and fell back to
# source-grepping the text instead.
#
# That was true only because /tmp/include never had an Arduino.h —
# install_system_stubs.py's own STUBS dict already assumes one exists
# (`#include <Arduino.h>` appears in half a dozen of its entries) and
# arduino_stub_template.h in this same directory *is* that stub, just
# never wired to anything. See install_system_stubs.py's own comment
# on this.
#
# This is a syntax/type check (-fsyntax-only against a stub SDK,
# -DARDUINO=10607 -DAUTOLINK_USE_ESP_TIMER, no AUTOLINK_HOST_TEST —
# the same macro shape a real arduino-cli build uses), not a
# behavioral test: the stubs return zero/no-op for everything, so
# this proves the code parses and type-checks against the real
# ESP-IDF/Arduino API surface it calls, not that it works. It is not
# a substitute for build/verify_build.sh; it is what closes the gap
# on every occasion verify_build.sh cannot run at all (this
# environment: no arduino-cli, no network egress to install one).
set -euo pipefail
cd "$(dirname "$0")/../../.."   # -> project root

python3 test/scripts/env/install_system_stubs.py > /dev/null

FILES=(
  src/al/hal/EspHal.cpp
  src/al/link/Link.cpp
  src/al/web/AutoLinkWebCore.cpp
  src/al/web/OtaCore.cpp
  src/al/web/handlers/AutoLinkWebHandlersData.cpp
  src/al/web/handlers/AutoLinkWebHandlersOta.cpp
  src/al/web/handlers/AutoLinkWebHttpd.cpp
  src/al/web/handlers/AutoLinkWebLifecycle.cpp
)

fail=0
for f in "${FILES[@]}"; do
  if g++ -std=c++14 -DARDUINO=10607 -DAUTOLINK_USE_ESP_TIMER \
         -I/tmp/include -Isrc -Isrc/al -fsyntax-only -x c++ "$f" \
         2> /tmp/host_syntax_check.$$; then
    echo "PASS: $f"
  else
    echo "FAIL: $f"
    sed 's/^/  /' /tmp/host_syntax_check.$$
    fail=1
  fi
  rm -f /tmp/host_syntax_check.$$
done

if [ "$fail" -ne 0 ]; then
  echo "host_syntax_check_arduino_tus: FAILED — see above" >&2
  exit 1
fi
echo "host_syntax_check_arduino_tus: all ${#FILES[@]} ARDUINO-only TUs type-check on host"
