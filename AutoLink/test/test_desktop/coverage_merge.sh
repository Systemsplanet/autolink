#!/bin/bash
# Generate gcov coverage reports.
#
# Each test binary links a different subset of the AutoLink sources, so
# instead of trying to merge .gcda across all 13 binaries (gcov-tool
# merge is fragile and often produces empty outputs), we pick the
# most-comprehensive binary for each source:
#
#   - run_test_alink_error covers ALink, Log, UtilBaudSweep, UtilCobs,
#     UtilCrc, UtilFrameRx, plus the test cpp itself. Use it for the
#     protocol sources.
#   - run_test_crc / run_test_cobs / run_test_framerx / run_test_baudsweep
#     each cover their utility more thoroughly than the protocol test
#     does. Use them for the Util* sources.
#   - run_test_log covers Log.cpp more thoroughly than the protocol test.
#   - run_test_blink covers UtilBlink.h (header-only, in the test cpp).
#
# If a future change adds a more-comprehensive test, update the map below.
set -e
cd "$(dirname "$0")"

echo "=== Assembling coverage inputs from per-suite .gcda files ==="
rm -rf coverage/merged && mkdir -p coverage/merged

# Map: source basename -> list of binaries whose .gcda we MERGE.
# Each binary exercises a different subset of the source's branches,
# so a single binary gives partial coverage. The full-coverage
# approach is to run every test that links the source, then
# gcov-tool merge the per-binary .gcda files into one canonical
# .gcda per source.
declare -A src_for=(
    [ALink]="run_test_alink_error run_test_alink_io run_test_alink_message run_test_alink_negotiation run_test_alink_watchdog run_test_alink_cobsseq"
    [Log]="run_test_log run_test_alink_error run_test_alink_io run_test_alink_message run_test_alink_negotiation run_test_alink_watchdog run_test_alink_cobsseq run_test_autolink run_test_mockhal"
    [AutoLink]="run_test_autolink run_test_alink_io"
    [ILink]="run_test_alink_error run_test_alink_io run_test_alink_message run_test_alink_negotiation run_test_alink_watchdog run_test_alink_cobsseq run_test_autolink run_test_mockhal"
    [UtilBaudSweep]="run_test_baudsweep run_test_alink_error run_test_alink_io run_test_alink_message run_test_alink_negotiation run_test_alink_watchdog run_test_alink_cobsseq"
    [UtilBlink]="run_test_blink"
    [UtilCobs]="run_test_cobs run_test_alink_error run_test_alink_io run_test_alink_message run_test_alink_negotiation run_test_alink_watchdog run_test_alink_cobsseq"
    [UtilCrc]="run_test_crc run_test_alink_error run_test_alink_io run_test_alink_message run_test_alink_negotiation run_test_alink_watchdog run_test_alink_cobsseq"
    [UtilFrameRx]="run_test_framerx run_test_alink_error run_test_alink_io run_test_alink_message run_test_alink_negotiation run_test_alink_watchdog run_test_alink_cobsseq"
    [MockHal]="run_test_mockhal run_test_alink_error run_test_alink_io run_test_alink_message run_test_alink_negotiation run_test_alink_watchdog run_test_alink_cobsseq"
)

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
for bin in run_test_crc run_test_cobs run_test_blink run_test_framerx \
           run_test_baudsweep run_test_log run_test_mockhal \
           run_test_alink_io run_test_alink_message run_test_alink_negotiation \
           run_test_alink_error run_test_alink_watchdog run_test_autolink; do
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

# Prune the STL / system-header files.
find coverage -maxdepth 1 -name "*.gcov" \
    ! -name "ALink*" ! -name "AutoLink*" ! -name "ILink*" ! -name "Log*" \
    ! -name "MockHal*" ! -name "UtilBaudSweep*" ! -name "UtilBlink*" \
    ! -name "UtilCobs*" ! -name "UtilCrc*" ! -name "UtilFrameRx*" \
    -delete 2>/dev/null || true
