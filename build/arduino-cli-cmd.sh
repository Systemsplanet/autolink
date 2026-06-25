#!/bin/bash
# arduino-cli-cmd.sh — thin wrapper around `arduino-cli`.
#
# Purpose:
#   - Ensures arduino-cli is installed (delegates to build_env.sh if
#     missing). Lets the rest of the workflow say
#     `bash build/arduino-cli-cmd.sh compile --fqbn ...` and just work,
#     even on a fresh machine where arduino-cli isn't on PATH yet.
#   - With no arguments, prints the arduino-cli version (sanity check
#     for the install). Useful in CI logs / as a smoke check.
#   - With arguments, passes them straight to arduino-cli.
#
# Why a wrapper (not a function in Make):
#   - The rule "always run arduino-cli through this wrapper" works
#     across make, CI, ad-hoc shell, AGENTS.md recipes, and the user's
#     own terminal. One file, one entry point.
#   - Lets us swap the install logic in one place if arduino-cli moves
#     off its current download URL (it already has twice in 2024).
#
# Usage:
#   bash build/arduino-cli-cmd.sh                     # print version
#   bash build/arduino-cli-cmd.sh compile --fqbn ...   # compile a sketch
#   bash build/arduino-cli-cmd.sh core list           # anything arduino-cli accepts
#
# This file lives in build/ alongside build_env.sh (which does the
# actual install) and verify_build.sh (which compiles the verify sketch).
# See AGENTS.md rule 17 for the "always use the wrapper" rule.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# 1. Install arduino-cli (and the esp32 core) if missing.
if ! command -v arduino-cli >/dev/null 2>&1; then
    echo "[arduino-cli-cmd.sh] arduino-cli not on PATH; running build_env.sh ..." >&2
    bash "$SCRIPT_DIR/build_env.sh"
fi

# 2. No-args path: print version and exit.
if [ $# -eq 0 ]; then
    arduino-cli version
    exit 0
fi

# 3. Pass-through.
exec arduino-cli "$@"
