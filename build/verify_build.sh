#!/bin/bash
# verify_build.sh — cross-compile build/verify_build.ino
# against the ESP32 Arduino target, like ArduinoDroid would.
#
# Compiles the library as-is, in-place. No staging, no flatten,
# no include-path hack — arduino-cli's --library flag handles
# subdirectories correctly.
#
# Required env: arduino-cli + esp32:esp32@3.3.5. The wrapper
# build/arduino-cli-cmd.sh handles install-on-demand; calling
# build_env.sh directly is no longer required.
#
# Usage:
#   ./build/verify_build.sh           # default FQBN
#   FQBN=esp32:esp32:esp32 ./build/verify_build.sh

set -e

FQBN="${FQBN:-esp32:esp32:firebeetle32}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SKETCH_DIR="$REPO_DIR/build/verify_build"

# Regenerate the seven src/al/web/generated/*.h files from the
# committed src/al/web/assets/ sources before compiling. The
# Arduino build's -I<lib_root>/src finds them at the staged path;
# if they're stale (any of the source assets change but the
# generated files don't), the cross-compile silently ships the
# old dashboard. The host test suite (make test) runs
# dashboard_assets.py as a pre-step too, but the
# cross-compile path needs the same guarantee.
python3 "$SCRIPT_DIR/dashboard_assets.py" --repo "$REPO_DIR"

# All arduino-cli calls go through the wrapper so install-on-demand
# works (no separate "run build_env.sh first" step).
# (Wrap as a bash function so 'bash <path>' stays two words even
# inside a quoted variable — bash otherwise treats the whole thing
# as one command name.)
cli() { bash "$SCRIPT_DIR/arduino-cli-cmd.sh" "$@"; }

cli cache clean
echo "Compiling $SKETCH_DIR/verify_build.ino against $FQBN ..."
cli compile \
    --fqbn "$FQBN" \
    --library "$REPO_DIR" \
    --warnings none \
    "$SKETCH_DIR"

echo ""
echo "OK. The library compiles cleanly against $FQBN."

# AL-A4: record that this actually happened. Nothing else in the
# tree proves whether verify_build.sh ever ran, or against what
# source, for a given release — pre_zip_check.sh's
# --require-crosscompile mode (AL-A1) reads this stamp and refuses
# to ship a zip without a passing stamp newer than every file under
# src/ and include/, unless the caller explicitly opts out with
# --allow-unverified (which itself gets written into the stamp so
# the release is traceable either way).
STAMP_DIR="$REPO_DIR/build/verify_build"
mkdir -p "$STAMP_DIR"
{
    echo "status=pass"
    echo "fqbn=$FQBN"
    echo "arduino_cli_version=$(cli version 2>/dev/null | head -1 || echo unknown)"
    echo "timestamp_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$STAMP_DIR/.last-pass"
echo "Stamped $STAMP_DIR/.last-pass"
