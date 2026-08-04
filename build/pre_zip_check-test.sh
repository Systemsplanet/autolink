#!/usr/bin/env bash
# Self-test for build/pre_zip_check.sh. Pins
# AGENTS.md rule 4 + rule 9: the pre-zip gate must
# actually fire (or pass) on the inputs we expect.
# A gate with no proof it fires is the same failure
# class as the "green/green" Pin 2 the
# head-of-tree pass filed. Run after every
# edit to pre_zip_check.sh; CI runs the full
# make-style gate that includes this script.
#
# Tests:
#   1. directory mode, complete staging root -> pass
#   2. directory mode, missing docs/todo.md -> fail
#   3. zip mode, complete archive -> pass
#   4. zip mode, archive missing docs/todo.md -> fail
#   5. zip mode, archive missing AGENTS.md (e.g. wrapper
#      folder eaten it) -> fail
#   6. zip mode, non-existent file -> fail (exit 1)
#   7. directory mode, non-existent path -> fail (exit 1)
#
# Exit: 0 if all 7 pass, 1 on any failure.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GATE="$HERE/pre_zip_check.sh"
if [ ! -f "$GATE" ]; then
    echo "pre_zip_check-test: gate not found at $GATE" >&2
    exit 1
fi
WORK=$(mktemp -d -t pre_zip_check_test.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

assert_exit() {
    local desc="$1"
    local want="$2"
    local got="$3"
    if [ "$want" = "$got" ]; then
        echo "  PASS: $desc (exit=$got)"
        pass=$((pass + 1))
    else
        echo "  FAIL: $desc (want exit=$want, got $got)"
        fail=$((fail + 1))
    fi
}

# --- Test 1: directory mode, complete staging root ---
mkdir -p "$WORK/good/docs"
touch "$WORK/good/AGENTS.md" "$WORK/good/README.md" \
      "$WORK/good/docs/Version.md" "$WORK/good/docs/todo.md"
"$GATE" "$WORK/good" > /dev/null 2>&1
assert_exit "dir mode, complete root" "0" "$?"

# --- Test 2: directory mode, missing docs/todo.md ---
mkdir -p "$WORK/bad/docs"
touch "$WORK/bad/AGENTS.md" "$WORK/bad/README.md" \
      "$WORK/bad/docs/Version.md"
# Missing docs/todo.md
"$GATE" "$WORK/bad" > /dev/null 2>&1
assert_exit "dir mode, missing docs/todo.md" "1" "$?"

# --- Test 3: zip mode, complete archive ---
mkdir -p "$WORK/good_zip/staging/docs"
: > "$WORK/good_zip/staging/AGENTS.md"
: > "$WORK/good_zip/staging/README.md"
mkdir -p "$WORK/good_zip/staging/docs"
: > "$WORK/good_zip/staging/docs/Version.md"
: > "$WORK/good_zip/staging/docs/todo.md"
(cd "$WORK/good_zip/staging" && zip -qr "$WORK/good.zip" .)
"$GATE" --zip "$WORK/good.zip" > /dev/null 2>&1
assert_exit "zip mode, complete archive" "0" "$?"

# --- Test 4: zip mode, archive missing docs/todo.md ---
mkdir -p "$WORK/bad_zip/staging/docs"
: > "$WORK/bad_zip/staging/AGENTS.md"
: > "$WORK/bad_zip/staging/README.md"
: > "$WORK/bad_zip/staging/docs/Version.md"
# Missing docs/todo.md
(cd "$WORK/bad_zip/staging" && zip -qr "$WORK/bad.zip" .)
"$GATE" --zip "$WORK/bad.zip" > /dev/null 2>&1
assert_exit "zip mode, missing docs/todo.md" "1" "$?"

# --- Test 5: zip mode, archive with wrapper folder (no
#     AGENTS.md at root) ---
mkdir -p "$WORK/wrap/staging/docs"
: > "$WORK/wrap/staging/AGENTS.md"
: > "$WORK/wrap/staging/README.md"
: > "$WORK/wrap/staging/docs/Version.md"
: > "$WORK/wrap/staging/docs/todo.md"
# Zip from parent (not staging/), so the archive has a
# wrapper folder 'staging/'.
(cd "$WORK/wrap" && zip -qr "$WORK/wrap.zip" staging)
"$GATE" --zip "$WORK/wrap.zip" > /dev/null 2>&1
assert_exit "zip mode, wrapper folder (no AGENTS.md at root)" "1" "$?"

# --- Test 6: zip mode, non-existent file ---
"$GATE" --zip "$WORK/no_such_zip_here.zip" > /dev/null 2>&1
assert_exit "zip mode, non-existent file" "1" "$?"

# --- Test 7: directory mode, non-existent path ---
"$GATE" "$WORK/no_such_dir" > /dev/null 2>&1
assert_exit "dir mode, non-existent path" "1" "$?"

echo
echo "=== pre_zip_check self-test: $pass pass, $fail fail ==="
if [ $fail -gt 0 ]; then
    exit 1
fi
exit 0
