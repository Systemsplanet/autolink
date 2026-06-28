#!/bin/bash
# Generate gcov coverage reports.
#
# Each test binary links a different subset of the AutoLink sources, so
# instead of trying to merge .gcda across all binaries (gcov-tool
# merge is fragile and often produces empty outputs), we pick the
# most-comprehensive binary for each source.
#
# The source-to-binary map used to live as a hardcoded `src_for` array
# in this script. That drifted from TEST_BINS in the test Makefile:
# adding a new suite there did not update this script, so coverage
# silently missed the new suite (see AGENTS.md rule 4).
#
# Fix: the Makefile generates a manifest (coverage/manifest.sh) that
# lists, for every library source basename, the set of run_test_*
# binaries that link it. The manifest is the single source of truth,
# derived from TEST_BINS and the per-suite build rules. This script
# sources the manifest and uses the resulting src_for_<basename>
# variables in place of the old hardcoded map. The list of test
# binaries is also passed via the manifest (TEST_BINS variable), so
# the test-file branch iterates the same set as the Makefile.
#
# Usage: coverage_merge.sh <manifest_path>
#
#   <manifest_path>  path to a shell-sourceable file produced by
#                    coverage_manifest.py, defining
#                      TEST_BINS="bin1 bin2 ..."
#                      src_for_<basename>="bin1 bin2 ..."
#                    for every library source referenced by at
#                    least one suite.
set -e
cd "$(dirname "$0")"

if [ $# -ne 1 ]; then
    echo "usage: coverage_merge.sh <manifest_path>" >&2
    exit 2
fi
MANIFEST="$1"
if [ ! -f "$MANIFEST" ]; then
    echo "coverage_merge.sh: manifest not found: $MANIFEST" >&2
    exit 2
fi

# shellcheck disable=SC1090
source "$MANIFEST"

if [ -z "$TEST_BINS" ]; then
    echo "coverage_merge.sh: manifest has no TEST_BINS" >&2
    exit 2
fi

echo "=== Assembling coverage inputs from per-suite .gcda files ==="
rm -rf coverage/merged && mkdir -p coverage/merged

# src_for is built from the manifest: every src_for_<basename>
# variable is a candidate. The map keys are the basenames g++
# uses for the .gcno/.gcda sidecars of each library source.
# Anything in src/ that a suite links ends up here automatically
# when the suite is added to TEST_BINS — no edit to this script.
# `declare -A` is required — without it bash treats src_for as
# an indexed array and string keys get coerced to integer indices.
declare -A src_for=()
for var in $(compgen -v | grep '^src_for_'); do
    base=${var#src_for_}
    # Indirection: ${!var} reads the variable named in $var.
    bins=${!var}
    src_for["$base"]=$bins
done

# Source: pick the .gcno from any binary (they're all identical for the
# same source). Then merge the .gcda from every binary that linked it
# using gcov-tool merge, producing a canonical .gcda per source with
# the union of all branches counted.
for src in "${!src_for[@]}"; do
    bins=${src_for[$src]}
    found_gcno=""
    for bin in $bins; do
        if [ -f "$bin-$src.gcno" ]; then
            cp "$bin-$src.gcno" "coverage/merged/$src.gcno"
            found_gcno="1"
            break
        fi
    done
    [ -n "$found_gcno" ] || continue
    # Collect every .gcda for this source.
    src_gcda=()
    for bin in $bins; do
        if [ -f "$bin-$src.gcda" ]; then
            cp "$bin-$src.gcda" "coverage/merged/${bin}_${src}.gcda"
            src_gcda+=("coverage/merged/${bin}_${src}.gcda")
        fi
    done
    # If there's only one, just rename it. If multiple, gcov-tool merge.
    if [ ${#src_gcda[@]} -eq 1 ]; then
        cp "${src_gcda[0]}" "coverage/merged/$src.gcda"
    elif [ ${#src_gcda[@]} -gt 1 ]; then
        # gcov-tool merge takes directories; put each gcda in its own dir.
        merge_in=()
        i=0
        for f in "${src_gcda[@]}"; do
            d="coverage/merged/_merge_${src}_${i}"
            mkdir -p "$d"
            cp "$f" "$d/${src}.gcda"
            cp "coverage/merged/$src.gcno" "$d/${src}.gcno"
            merge_in+=("$d")
            i=$((i+1))
        done
        # Merge pairwise. gcov-tool merge only takes 2 dirs, so chain.
        out_dir="coverage/merged/_merge_${src}_out"
        mkdir -p "$out_dir"
        cp "coverage/merged/$src.gcno" "$out_dir/${src}.gcno"
        current="${merge_in[0]}"
        for ((j=1; j<${#merge_in[@]}; j++)); do
            next="${merge_in[$j]}"
            tmp="coverage/merged/_merge_${src}_tmp_${j}"
            mkdir -p "$tmp"
            cp "coverage/merged/$src.gcno" "$tmp/${src}.gcno"
            gcov-tool merge -o "$tmp" "$current" "$next" 2>/dev/null || cp "$current/${src}.gcda" "$tmp/${src}.gcda"
            current="$tmp"
        done
        cp "$current/${src}.gcda" "coverage/merged/$src.gcda"
    fi
    # Clean up the per-binary copies.
    for bin in $bins; do
        rm -f "coverage/merged/${bin}_${src}.gcda"
        rm -rf "coverage/merged/_merge_${src}_"* 2>/dev/null
    done
done

# Test files: one .gcno + .gcda per test, renamed to avoid collisions.
# Iterate TEST_BINS from the manifest rather than a hardcoded list —
# adding a new suite to TEST_BINS automatically includes its test-cpp
# coverage here.
for bin in $TEST_BINS; do
    for ext in gcno gcda; do
        for f in "$bin"-*."$ext"; do
            [ -e "$f" ] || continue
            base=${f%%-*}
            name=${f#*-}
            cp "$f" "coverage/merged/${bin}_${name}"
        done
    done
done

echo "=== Generating coverage report ==="
# Only process the canonical source .gcno files. Each test binary's
# .gcno also references the src/ sources and would overwrite the
# canonical gcov with the test's partial view, so we skip them here.
# Test cpp coverage is implicit (the tests run to completion; the
# assert() lines are exercised).
for f in coverage/merged/*.gcno; do
    base=$(basename "$f" .gcno)
    case "$base" in
        run_test_*) ;;
        *) gcov -b -o coverage/merged "$f" > /dev/null 2>&1 ;;
    esac
done
mv -f *.gcov coverage/ 2>/dev/null || true

# Prune STL / system-header files. The set of library basenames we
# care about is the same set of keys in src_for; we feed those into
# find -name patterns so that adding a new library source updates
# the keep-list automatically.
keep_pat=()
for src in "${!src_for[@]}"; do
    if [ ${#keep_pat[@]} -gt 0 ]; then
        keep_pat+=("-o")
    fi
    keep_pat+=("-name" "${src}*")
done
if [ ${#keep_pat[@]} -gt 0 ]; then
    find coverage -maxdepth 1 -name "*.gcov" \
        \( "${keep_pat[@]}" \) -print > /tmp/.keep_gcovs 2>/dev/null || true
    # Delete everything that isn't in the keep list.
    if [ -s /tmp/.keep_gcovs ]; then
        find coverage -maxdepth 1 -name "*.gcov" \
            | sort > /tmp/.all_gcovs
        sort -u /tmp/.keep_gcovs > /tmp/.keep_sorted
        comm -23 /tmp/.all_gcovs /tmp/.keep_sorted \
            | xargs -r rm -f 2>/dev/null || true
    fi
    rm -f /tmp/.keep_gcovs /tmp/.all_gcovs /tmp/.keep_sorted
fi
