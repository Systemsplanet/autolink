#!/usr/bin/env bash
# Pre-zip gate: hard check that the deliverable
# (zip or staging root) carries the four files
# every release needs (AGENTS.md / README.md /
# docs/Version.md / docs/todo.md). A non-empty
# "missing" set means a file the user depends on
# is absent even if the cross-compile passed —
# AGENTS rule 9 (delivery). Wired into the
# orchestrator's pre-zip step per AGENTS.md
# Delivery rules.
#
# Two modes:
#   bash build/pre_zip_check.sh <staging_root>
#       Check the staging directory on disk.
#   bash build/pre_zip_check.sh --zip <path.zip>
#       Check the archive (read `unzip -l` and
#       assert the four entries are present in
#       the archive, at the archive root, with
#       no wrapper folder). The zip mode is the
#       load-bearing one: a prior release shipped
#       a file missing from the *archive*, not
#       from the staging root — a staging root
#       can be complete and the zip still lose
#       the file (wrong `cd`, a stale exclude
#       pattern, a `zip -x` glob that over-
#       matches).
#
# Exit: 0 on pass, 1 on any missing file, 2 on
# usage error.
set -u

required=(AGENTS.md README.md docs/Version.md docs/todo.md)

if [ "${1:-}" = "--zip" ]; then
    if [ $# -lt 2 ]; then
        echo "pre_zip_check: --zip requires a path" >&2
        echo "usage: bash build/pre_zip_check.sh --zip <path.zip>" >&2
        exit 2
    fi
    zip_path="$2"
    if [ ! -f "$zip_path" ]; then
        echo "pre_zip_check: $zip_path is not a file" >&2
        exit 1
    fi
    # unzip -l output is `Length  Date  Time  Name`, one
    # entry per line. Skip the first 3 header lines and
    # the trailing 2 separator/total lines. Strip
    # directory components; we want entries that match
    # `<required>` exactly (the four files must be at
    # the archive root, AGENTS rule 8 — no wrapper
    # folder).
    list=$(unzip -l "$zip_path" 2>/dev/null | awk 'NR>3 {print $NF}' \
        | sed -n '/^---/!p' | sed -n '/^[[:space:]]*[0-9]\+/!p')
    missing=()
    for f in "${required[@]}"; do
        # Match the file at the archive root. We do
        # not want a false positive on `docs/AGENTS.md`
        # or any other nested path. `grep -Fxq` does an
        # exact-line match.
        if ! printf '%s\n' "$list" | grep -Fxq "$f"; then
            missing+=("$f")
        fi
    done
    if [ ${#missing[@]} -gt 0 ]; then
        echo "pre_zip_check: $zip_path is missing required entries:" >&2
        for f in "${missing[@]}"; do
            echo "  $f" >&2
        done
        echo "AGENTS.md rule 9 (delivery): every zip must carry these." >&2
        exit 1
    fi
    echo "pre_zip_check: $zip_path OK (${#required[@]} required entries present)"
    exit 0
fi

# Directory mode (staging-time use).
root="${1:-staging}"
if [ ! -d "$root" ]; then
    echo "pre_zip_check: $root is not a directory" >&2
    exit 1
fi
missing=()
for f in "${required[@]}"; do
    if [ ! -f "$root/$f" ]; then
        missing+=("$f")
    fi
done
if [ ${#missing[@]} -gt 0 ]; then
    echo "pre_zip_check: $root is missing required files:" >&2
    for f in "${missing[@]}"; do
        echo "  $f" >&2
    done
    echo "AGENTS.md rule 9 (delivery): every zip must carry these." >&2
    exit 1
fi
echo "pre_zip_check: $root OK (${#required[@]} required files present)"
exit 0
