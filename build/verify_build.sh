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

# Regenerate AutoLinkWebHtml.h from the committed
# dashboard.css / dashboard.js / dashboard_html_part_*.html
# sources before compiling. The Arduino build's
# -I<lib_root>/src finds the header at the staged path;
# if it's stale (any of the source assets change but the
# header doesn't), the cross-compile silently ships the
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
