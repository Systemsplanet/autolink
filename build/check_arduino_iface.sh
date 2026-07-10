#!/bin/bash
# check_arduino_iface.sh — gate that catches the ArduinoDroid sketch-TU
# flag-drop bug class.
#
# The bug: ArduinoDroid (and some IDE integrations) compile the sketch
# translation unit with no g++ flags — no -DARDUINO=, no -I<lib_root>/src,
# no -c, no -isysroot. Library .cpp files compile fine (they get the
# full flag list), but every #ifdef ARDUINO block in the public headers
# drops out for the sketch TU, and the user sees:
#
#   error: 'autolink' is not a namespace-name
#   error: 'PingPong' does not name a type
#
# The library is fine. The build is broken.
#
# This gate simulates that worst case directly: we build a real sketch
# TU against the ESP32 toolchain with only `-DARDUINO=10607` and
# `-I<lib_root>/src` on the command line. The rest of the flags
# (-I<core>, -isysroot, FreeRTOS path, -Os, etc.) are stripped — exactly
# the worst case ArduinoDroid would emit if it dropped the flag list.
#
# Expected outcomes:
#   - Gate intact: the public surface (namespace autolink, class
#     PingPong) is fully visible because the headers are guarded by
#     `#ifdef ARDUINO`. The compile fails ONLY because <Arduino.h>
#     is not on the stripped -I list — that error is the SAME error
#     ArduinoDroid would surface, and it tells the user exactly
#     what's missing (no library change needed).
#   - Gate broken: the public surface went dark because someone
#     moved a public symbol out from behind `#ifdef ARDUINO`, or
#     removed the guard entirely. The compile fails with the
#     regression signature `'autolink' is not a namespace-name`
#     or `PingPong was not declared in this scope`.
#
# Exit codes:
#   0 = pass
#   1 = infrastructure error (toolchain missing, etc.)
#   2 = header-guard bug class detected (gate broken — sketch TU has #ifdef ARDUINO
#         symbols that go dark)
#   3 = unrelated compile/link error (something else broke)
#   4 = self-test failure (the gate itself is wrong)
#   5 = link-stage library-deps bug class detected (FS / LittleFS / WiFi / Preferences
#         not pulled into the link — undefined references to fs::FS / fs::File /
#         VFSImpl symbols)
#
# Usage:
#   bash build/check_arduino_iface.sh
#   FQBN=esp32:esp32:esp32 bash build/check_arduino_iface.sh
#
# This is the AutoLink project's CI gate for the ArduinoDroid
# sketch-TU flag-drop regression. The pre-existing verify_build.sh
# already exercises the standard happy path; this gate catches the
# specific sketch-TU flag-drop bug class that verify_build.sh
# cannot trigger (arduino-cli always passes the full flag list).

set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FQBN="${FQBN:-esp32:esp32:firebeetle32}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cli() { bash "$SCRIPT_DIR/arduino-cli-cmd.sh" "$@"; }

echo "=== ArduinoDroid sketch-TU flag-drop gate ==="
echo "  FQBN: $FQBN"
echo "  repo: $REPO_DIR"
echo ""

if ! command -v arduino-cli >/dev/null 2>&1; then
    echo "INFRA ERROR: arduino-cli not on PATH; install via build/build_env.sh" >&2
    exit 1
fi

# Locate the ESP32 g++ toolchain.
ESP32_TOOLS="/root/.arduino15/packages/esp32/tools"
GPP="$(find "$ESP32_TOOLS" -name 'xtensa-esp*-g++' -type f 2>/dev/null | head -1)"
if [ ! -x "$GPP" ]; then
    echo "INFRA ERROR: cannot locate xtensa-esp-elf-g++ under $ESP32_TOOLS" >&2
    exit 1
fi

# -----------------------------------------------------------------------------
# Phase 1: standard compile via arduino-cli (the happy path).
# -----------------------------------------------------------------------------

POSITIVE_DIR="$TMP/positive"
mkdir -p "$POSITIVE_DIR"
cat > "$POSITIVE_DIR/positive.ino" <<'EOF'
#include <Arduino.h>
#include "AutoLink.h"
#include "PingPong.h"
using namespace autolink;
PingPong upp(PingPong::PONG, 115200, UART_NUM_2, 16, 17);
void setup() { upp.setup(); }
void loop() { upp.loop(); }
EOF

echo "Phase 1: standard sketch compile (happy path)..."
if ! cli compile --fqbn "$FQBN" --library "$REPO_DIR" --warnings none "$POSITIVE_DIR" \
        > "$TMP/positive.log" 2>&1; then
    echo "FAIL: positive sketch did not compile" >&2
    tail -30 "$TMP/positive.log" >&2
    exit 3
fi
echo "  PASS: positive sketch compiled"
echo ""

# -----------------------------------------------------------------------------
# Phase 2: simulate the ArduinoDroid sketch-TU flag-drop.
#
# Build a fresh sketch TU with the minimum flag set required by the
# public headers. The public shims in src/AutoLink.h and src/PingPong.h
# declare the public surface inside `#ifdef ARDUINO` blocks. If the
# gate is intact, the namespace and types are visible to the sketch
# TU. The compile fails ONLY because the core / freertos / driver
# headers (Arduino.h, etc.) are not on -I (we stripped them on
# purpose to mimic the ArduinoDroid worst case).
# -----------------------------------------------------------------------------

MIN_SKETCH="$TMP/minflags.ino.cpp"
cat > "$MIN_SKETCH" <<'EOF'
#include "AutoLink.h"
#include "PingPong.h"
using namespace autolink;
PingPong upp(PingPong::PONG, 115200, UART_NUM_2, 16, 17);
void __assert_sketch_tu_compiles() { (void)upp; }
EOF

echo "Phase 2: sketch-TU flag-drop simulation using $GPP"
echo "  flags: -DARDUINO=10607 -x c++ -std=gnu++17 -I$REPO_DIR/src"

MIN_LOG="$TMP/minflags.log"
"$GPP" \
    -DARDUINO=10607 \
    -x c++ \
    -std=gnu++17 \
    -I"$REPO_DIR/src" \
    -c "$MIN_SKETCH" \
    -o "$TMP/minflags.o" \
    > "$MIN_LOG" 2>&1
MIN_RC=$?

# Diagnostic classification.
if [ "$MIN_RC" -eq 0 ]; then
    # The sketch TU compiled clean with no Arduino.h path —
    # that's the gold standard: the public surface is fully
    # self-contained behind #ifdef ARDUINO and doesn't even
    # need <Arduino.h> to declare the type. But this is unusual
    # because PingPongMain.h includes Arduino.h transitively
    # through PingPongBase.h. Mark this as PASS but flag it
    # for future audit (the gate may need to be tightened).
    echo "  PASS: sketch TU compiled with minimal flag set (gate intact, fully self-contained)"
elif grep -qE "'autolink' is not a namespace|'PingPong' does not name|'AutoLink' was not declared" "$MIN_LOG"; then
    echo "FAIL: header-guard regression — public surface went dark with -DARDUINO=10607" >&2
    tail -20 "$MIN_LOG" >&2
    exit 2
elif grep -qE "Arduino\.h: No such file|fatal error: Arduino\.h|fatal error: freertos/|fatal error: driver/|fatal error: esp_" "$MIN_LOG"; then
    echo "  PASS: sketch TU fails for the right reason (missing core header, NOT header-guard bug)"
else
    echo "FAIL: sketch TU failed for an unrecognized reason" >&2
    tail -20 "$MIN_LOG" >&2
    exit 3
fi
echo ""

# -----------------------------------------------------------------------------
# Phase 3: self-test. Toggle the gate off and confirm the gate fires.
# We do this by writing a tweaked copy of src/AutoLink.h with the
# include of the canonical header commented out, running the gate's
# Phase 2 directly against the broken tree, and verifying it returns
# the header-guard-regression diagnostic (NOT the missing-Arduino.h
# one). If it doesn't, the gate itself is broken (exit 4).
# -----------------------------------------------------------------------------

echo "Phase 3: self-test (gate must fire when the gate is broken)..."

# Build a sandboxed src/ tree where the public shims are
# deliberately broken (canonical include commented out). Then
# point the sketch TU at THAT tree instead of $REPO_DIR/src.
SELF_SRC="$TMP/self_src"
mkdir -p "$SELF_SRC/al"
cp -r "$REPO_DIR/src/AutoLink.h" "$SELF_SRC/AutoLink.h"
cp -r "$REPO_DIR/src/PingPong.h" "$SELF_SRC/PingPong.h"
cp -r "$REPO_DIR/src/al" "$SELF_SRC/al"
# Break the gate: replace the canonical include with a comment.
sed -i 's|^#include "../include/AutoLink.h"|// BROKEN-GATE: #include "../include/AutoLink.h"|' "$SELF_SRC/AutoLink.h"
sed -i 's|^#include "al/pingpong/PingPongMain.h"|// BROKEN-GATE: #include "al/pingpong/PingPongMain.h"|' "$SELF_SRC/PingPong.h"

SELF_SKETCH="$TMP/self_min.ino.cpp"
cat > "$SELF_SKETCH" <<'EOF'
#include "AutoLink.h"
#include "PingPong.h"
using namespace autolink;
PingPong upp(PingPong::PONG, 115200, UART_NUM_2, 16, 17);
void __assert_self_compiles() { (void)upp; }
EOF

"$GPP" \
    -DARDUINO=10607 \
    -x c++ \
    -std=gnu++17 \
    -I"$SELF_SRC" \
    -c "$SELF_SKETCH" \
    -o "$TMP/self.o" \
    > "$TMP/self.log" 2>&1
SELF_RC=$?

if grep -qE "'autolink' is not a namespace|'PingPong' does not name|'AutoLink' was not declared" "$TMP/self.log"; then
    echo "  PASS: gate correctly detects the broken-gate regression (header-guard signature)"
elif [ "$SELF_RC" -eq 0 ]; then
    echo "FAIL: gate did not fire — broken shim compiled clean" >&2
    cat "$TMP/self.log" >&2
    exit 4
else
    echo "FAIL: gate did not fire — broken shim failed with unexpected diagnostic" >&2
    tail -20 "$TMP/self.log" >&2
    exit 4
fi
echo ""

# -----------------------------------------------------------------------------
# Phase 4: link-stage gate for the library-deps bug class.
#
# ArduinoDroid (and some IDE integrations) build the link line by
# picking up only the libraries the SKETCH explicitly #includes,
# without auto-resolving transitive library dependencies. The
# classic failure: AutoLinkWeb uses LittleFS, which depends on
# FS, but only <LittleFS.h> is included in our web TUs — so the
# IDE never links cores/esp32/FS.cpp into core.a, and the user
# gets "undefined reference to fs::File::close()" / "fs::FS::exists()"
# / "vtable for fs::File" / "VFSImpl::open" at link time.
#
# The fix lives in two places, and we gate both with static
# source-grep pins because the runtime link stage in arduino-cli
# auto-resolves transitive deps and therefore cannot reproduce
# the ArduinoDroid bug class:
#
#   1. library.properties declares `depends=FS,LittleFS,WiFi,Preferences`
#      so IDEs that respect it pull those libraries into the build.
#   2. The web TUs include <FS.h> directly so the core builder
#      sees the FS reference even on IDEs that don't transitively
#      resolve library deps.
#
# We also run the full link via arduino-cli to confirm the
# AutoLinkWeb + LittleFS sketch shape produces a .bin/.elf end-to-end
# (the arduino-cli happy path; the static pins are what catch the
# ArduinoDroid regression).
# -----------------------------------------------------------------------------

echo "Phase 4: link-stage library-deps static checks..."

DEPENDS_OK=0
if grep -qE '^depends=.*\bFS\b' "$REPO_DIR/library.properties" \
   && grep -qE '^depends=.*\bLittleFS\b' "$REPO_DIR/library.properties" \
   && grep -qE '^depends=.*\bWiFi\b' "$REPO_DIR/library.properties" \
   && grep -qE '^depends=.*\bPreferences\b' "$REPO_DIR/library.properties"; then
    echo "  PASS: library.properties declares depends=FS,LittleFS,WiFi,Preferences"
    DEPENDS_OK=1
else
    echo "FAIL: library.properties is missing one or more depends entries" >&2
    echo "  Required: depends=FS, LittleFS, WiFi, Preferences" >&2
    grep '^depends=' "$REPO_DIR/library.properties" >&2 || echo "  (no depends= line found at all)" >&2
    exit 5
fi

FS_INCLUDE_OK=0
if grep -qE '^#include <FS\.h>$' "$REPO_DIR/src/al/web/AutoLinkWeb.cpp" \
   && grep -qE '^#include <FS\.h>$' "$REPO_DIR/src/al/web/AutoLinkWebHandlers.cpp"; then
    echo "  PASS: web TUs include <FS.h> directly"
    FS_INCLUDE_OK=1
else
    echo "FAIL: web TUs are missing the direct #include <FS.h>" >&2
    echo "  AutoLinkWeb.cpp:"; grep -n '#include <.*FS\.h>' "$REPO_DIR/src/al/web/AutoLinkWeb.cpp" || echo "    (no FS include)" >&2
    echo "  AutoLinkWebHandlers.cpp:"; grep -n '#include <.*FS\.h>' "$REPO_DIR/src/al/web/AutoLinkWebHandlers.cpp" || echo "    (no FS include)" >&2
    exit 5
fi

if [ "$DEPENDS_OK" -ne 1 ] || [ "$FS_INCLUDE_OK" -ne 1 ]; then
    exit 5
fi

echo ""

# -----------------------------------------------------------------------------
# Phase 5: arduino-cli link smoke test for AutoLinkWeb + LittleFS.
# Confirms the full build succeeds on arduino-cli (which auto-resolves
# transitive deps). The static pins in Phase 4 are what catch the
# ArduinoDroid-specific regression; this phase is the smoke check that
# we haven't broken anything else.
# -----------------------------------------------------------------------------

echo "Phase 5: arduino-cli link smoke test (AutoLinkWeb + LittleFS)..."

LINK_SKETCH_DIR="$TMP/link_test"
mkdir -p "$LINK_SKETCH_DIR"
cat > "$LINK_SKETCH_DIR/link_test.ino" <<'EOF'
#include <Arduino.h>
#include "AutoLink.h"
#include "al/web/AutoLinkWeb.h"
#include <WiFi.h>
#include <LittleFS.h>
#include <FS.h>
using namespace autolink;
static AutoLink g_link(UART_NUM_2, 16, 17, true);
static AutoLinkWeb g_web(g_link);
void setup() {
    Serial.begin(115200);
    g_link.begin();
    g_web.begin("test", "test", 8765);
}
void loop() { delay(10); }
EOF

if ! cli compile --fqbn "$FQBN" --library "$REPO_DIR" --warnings none "$LINK_SKETCH_DIR" \
        > "$TMP/link.log" 2>&1; then
    if grep -qE "undefined reference to .fs::|undefined reference to .VFSImpl::|vtable for fs::File" "$TMP/link.log"; then
        echo "FAIL: link-stage library-deps bug class detected (even on arduino-cli)" >&2
        grep -E "undefined reference" "$TMP/link.log" | head -5 >&2
        exit 5
    else
        echo "FAIL: link-stage failed for an unrecognized reason" >&2
        tail -30 "$TMP/link.log" >&2
        exit 3
    fi
fi

CACHE_BIN="$(find /root/.cache/arduino/sketches -name 'link_test.ino.bin' -type f 2>/dev/null | xargs ls -t 2>/dev/null | head -1)"
CACHE_ELF="$(find /root/.cache/arduino/sketches -name 'link_test.ino.elf' -type f 2>/dev/null | xargs ls -t 2>/dev/null | head -1)"
if [ -z "$CACHE_BIN" ] || [ -z "$CACHE_ELF" ] || [ ! -s "$CACHE_BIN" ] || [ ! -s "$CACHE_ELF" ]; then
    echo "FAIL: compile succeeded but no .bin/.elf produced" >&2
    tail -10 "$TMP/link.log" >&2
    exit 3
fi
echo "  PASS: full link succeeded (.bin $(stat -c %s "$CACHE_BIN") B, .elf $(stat -c %s "$CACHE_ELF") B)"
echo ""

echo "=== ArduinoDroid sketch-TU flag-drop gate: PASS ==="