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

# Map: source basename -> binary whose .gcda we use.
declare -A src_for=(
    [ALink]="run_test_alink_error"
    [Log]="run_test_log"
    [AutoLink]="run_test_alink_io"
    [ILink]="run_test_alink_error"
    [UtilBaudSweep]="run_test_baudsweep"
    [UtilBlink]="run_test_blink"
    [UtilCobs]="run_test_cobs"
    [UtilCrc]="run_test_crc"
    [UtilFrameRx]="run_test_framerx"
    [MockHal]="run_test_mockhal"
)

# Source: one .gcno + one .gcda per source. Use a renamed .gcno/.gcda
# (without the binary prefix) so gcov's output doesn't collide across
# sources.
for src in "${!src_for[@]}"; do
    bin=${src_for[$src]}
    if [ -f "$bin-$src.gcno" ] && [ -f "$bin-$src.gcda" ]; then
        cp "$bin-$src.gcno" "coverage/merged/$src.gcno"
        cp "$bin-$src.gcda" "coverage/merged/$src.gcda"
    fi
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
