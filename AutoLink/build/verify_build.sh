#!/bin/bash
# verify_build.sh — cross-compile verify_build/verify_build.ino
# against the ESP32 Arduino target, like ArduinoDroid would.
#
# Compiles the library as-is, in-place. No staging, no flatten,
# no include-path hack — arduino-cli's --library flag handles
# subdirectories correctly.
#
# Required env: arduino-cli + esp32:esp32@3.3.5 (run build/build_env.sh).
#
# Usage:
#   ./build/verify_build.sh           # default FQBN
#   FQBN=esp32:esp32:esp32 ./build/verify_build.sh

set -e

FQBN="${FQBN:-esp32:esp32:firebeetle32}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SKETCH_DIR="$REPO_DIR/verify_build"

if ! command -v arduino-cli >/dev/null 2>&1; then
    echo "arduino-cli not found. Run ./build/build_env.sh first." >&2
    exit 1
fi

arduino-cli cache clean
echo "Compiling $SKETCH_DIR/verify_build.ino against $FQBN ..."
arduino-cli compile \
    --fqbn "$FQBN" \
    --library "$REPO_DIR" \
    --warnings none \
    "$SKETCH_DIR"

echo ""
echo "OK. The library compiles cleanly against $FQBN."
