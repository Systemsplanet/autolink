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
# Tests (one per required entry, plus error-path
# coverage):
#   1. directory mode, complete staging root -> pass
#   2. directory mode, missing docs/Version.md -> fail
#   3. directory mode, missing AGENTS.md -> fail
#   4. directory mode, missing README.md -> fail
#   5. zip mode, complete archive -> pass
#   6. zip mode, archive missing docs/Version.md -> fail
#   7. zip mode, archive missing AGENTS.md -> fail
#   8. zip mode, archive missing README.md -> fail
#   9. zip mode, archive with wrapper folder (no
#      AGENTS.md at root) -> fail
#  10. zip mode, non-existent file -> fail (exit 1)
#  11. directory mode, non-existent path -> fail (exit 1)
#  12-14. strip-list enforcement (zip/dir mode, stale binaries)
#  15. directory mode, DOTLESS staging root with a stale test
#      binary -> fail (exit 1) — AL92-8 regression guard
#  16-19. AL-A1/AL-A4: --require-crosscompile (no stamp -> fail,
#      --allow-unverified -> pass with a WARNING, stale stamp ->
#      fail, fresh stamp -> pass)
#  20-21. AL-C1: version.py check wired in (over DEFAULT_KEEP ->
#      fail, at/under -> pass)
#  22-23. AL-E1: rule20a_check wired in (new violation, empty
#      baseline -> fail; same violation covered by baseline -> pass)
#
# Exit: 0 if all 23 pass, 1 on any failure.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GATE="$HERE/pre_zip_check.sh"

# AL-C1: pre_zip_check now also runs `build/version.py check`
# against any docs/Version.md it finds, so the two PASS-expected
# fixtures below that need version.py to accept the file (rather
# than fail on the empty-file header check) get real minimal
# content instead of `: > file`.
VERSION_MD_STUB='# 📅 AutoLink Version History

All releases, most recent first.
## v0.0.1

Stub entry for pre_zip_check-test.sh fixtures.
'
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
mkdir -p "$WORK/good/test/test_desktop" "$WORK/good/test/common" "$WORK/good/test/scripts/coverage"
touch "$WORK/good/AGENTS.md" "$WORK/good/README.md" \
      "$WORK/good/test/Makefile" "$WORK/good/test/test_desktop/Makefile" \
      "$WORK/good/test/common/MockHal.h" \
      "$WORK/good/test/scripts/coverage/coverage_manifest.py"
printf '%s' "$VERSION_MD_STUB" > "$WORK/good/docs/Version.md"
"$GATE" "$WORK/good" > /dev/null 2>&1
assert_exit "dir mode, complete root" "0" "$?"

# --- Test 2: directory mode, missing docs/Version.md ---
mkdir -p "$WORK/bad_ver/docs" "$WORK/bad_ver/test/test_desktop" "$WORK/bad_ver/test/common" "$WORK/bad_ver/test/scripts/coverage"
touch "$WORK/bad_ver/AGENTS.md" "$WORK/bad_ver/README.md" \
      "$WORK/bad_ver/test/Makefile" "$WORK/bad_ver/test/test_desktop/Makefile" \
      "$WORK/bad_ver/test/common/MockHal.h" \
      "$WORK/bad_ver/test/scripts/coverage/coverage_manifest.py"
# Missing docs/Version.md
"$GATE" "$WORK/bad_ver" > /dev/null 2>&1
assert_exit "dir mode, missing docs/Version.md" "1" "$?"

# --- Test 3: directory mode, missing AGENTS.md ---
mkdir -p "$WORK/bad_agents/docs" "$WORK/bad_agents/test/test_desktop" "$WORK/bad_agents/test/common" "$WORK/bad_agents/test/scripts/coverage"
touch "$WORK/bad_agents/README.md" \
      "$WORK/bad_agents/docs/Version.md" \
      "$WORK/bad_agents/test/Makefile" "$WORK/bad_agents/test/test_desktop/Makefile" \
      "$WORK/bad_agents/test/common/MockHal.h" \
      "$WORK/bad_agents/test/scripts/coverage/coverage_manifest.py"
# Missing AGENTS.md
"$GATE" "$WORK/bad_agents" > /dev/null 2>&1
assert_exit "dir mode, missing AGENTS.md" "1" "$?"

# --- Test 4: directory mode, missing README.md ---
mkdir -p "$WORK/bad_readme/docs" "$WORK/bad_readme/test/test_desktop" "$WORK/bad_readme/test/common" "$WORK/bad_readme/test/scripts/coverage"
touch "$WORK/bad_readme/AGENTS.md" \
      "$WORK/bad_readme/docs/Version.md" \
      "$WORK/bad_readme/test/Makefile" "$WORK/bad_readme/test/test_desktop/Makefile" \
      "$WORK/bad_readme/test/common/MockHal.h" \
      "$WORK/bad_readme/test/scripts/coverage/coverage_manifest.py"
# Missing README.md
"$GATE" "$WORK/bad_readme" > /dev/null 2>&1
assert_exit "dir mode, missing README.md" "1" "$?"

# --- Test 5: zip mode, complete archive ---
mkdir -p "$WORK/good_zip/staging/docs" \
         "$WORK/good_zip/staging/test/test_desktop" \
         "$WORK/good_zip/staging/test/common" \
         "$WORK/good_zip/staging/test/scripts/coverage"
: > "$WORK/good_zip/staging/AGENTS.md"
: > "$WORK/good_zip/staging/README.md"
printf '%s' "$VERSION_MD_STUB" > "$WORK/good_zip/staging/docs/Version.md"
: > "$WORK/good_zip/staging/test/Makefile"
: > "$WORK/good_zip/staging/test/test_desktop/Makefile"
: > "$WORK/good_zip/staging/test/common/MockHal.h"
: > "$WORK/good_zip/staging/test/scripts/coverage/coverage_manifest.py"
(cd "$WORK/good_zip/staging" && zip -qr "$WORK/good.zip" .)
"$GATE" --zip "$WORK/good.zip" > /dev/null 2>&1
assert_exit "zip mode, complete archive" "0" "$?"

# --- Test 6: zip mode, archive missing docs/Version.md ---
mkdir -p "$WORK/bad_zip_ver/staging/docs" \
         "$WORK/bad_zip_ver/staging/test/test_desktop" \
         "$WORK/bad_zip_ver/staging/test/common" \
         "$WORK/bad_zip_ver/staging/test/scripts/coverage"
: > "$WORK/bad_zip_ver/staging/AGENTS.md"
: > "$WORK/bad_zip_ver/staging/README.md"
: > "$WORK/bad_zip_ver/staging/test/Makefile"
: > "$WORK/bad_zip_ver/staging/test/test_desktop/Makefile"
: > "$WORK/bad_zip_ver/staging/test/common/MockHal.h"
: > "$WORK/bad_zip_ver/staging/test/scripts/coverage/coverage_manifest.py"
# Missing docs/Version.md
(cd "$WORK/bad_zip_ver/staging" && zip -qr "$WORK/bad_ver.zip" .)
"$GATE" --zip "$WORK/bad_ver.zip" > /dev/null 2>&1
assert_exit "zip mode, missing docs/Version.md" "1" "$?"

# --- Test 7: zip mode, archive missing AGENTS.md ---
mkdir -p "$WORK/bad_zip_agents/staging/docs" \
         "$WORK/bad_zip_agents/staging/test/test_desktop" \
         "$WORK/bad_zip_agents/staging/test/common" \
         "$WORK/bad_zip_agents/staging/test/scripts/coverage"
: > "$WORK/bad_zip_agents/staging/README.md"
: > "$WORK/bad_zip_agents/staging/docs/Version.md"
: > "$WORK/bad_zip_agents/staging/test/Makefile"
: > "$WORK/bad_zip_agents/staging/test/test_desktop/Makefile"
: > "$WORK/bad_zip_agents/staging/test/common/MockHal.h"
: > "$WORK/bad_zip_agents/staging/test/scripts/coverage/coverage_manifest.py"
# Missing AGENTS.md
(cd "$WORK/bad_zip_agents/staging" && zip -qr "$WORK/bad_agents.zip" .)
"$GATE" --zip "$WORK/bad_agents.zip" > /dev/null 2>&1
assert_exit "zip mode, missing AGENTS.md" "1" "$?"

# --- Test 8: zip mode, archive missing README.md ---
mkdir -p "$WORK/bad_zip_readme/staging/docs" \
         "$WORK/bad_zip_readme/staging/test/test_desktop" \
         "$WORK/bad_zip_readme/staging/test/common" \
         "$WORK/bad_zip_readme/staging/test/scripts/coverage"
: > "$WORK/bad_zip_readme/staging/AGENTS.md"
: > "$WORK/bad_zip_readme/staging/docs/Version.md"
: > "$WORK/bad_zip_readme/staging/test/Makefile"
: > "$WORK/bad_zip_readme/staging/test/test_desktop/Makefile"
: > "$WORK/bad_zip_readme/staging/test/common/MockHal.h"
: > "$WORK/bad_zip_readme/staging/test/scripts/coverage/coverage_manifest.py"
# Missing README.md
(cd "$WORK/bad_zip_readme/staging" && zip -qr "$WORK/bad_readme.zip" .)
"$GATE" --zip "$WORK/bad_readme.zip" > /dev/null 2>&1
assert_exit "zip mode, missing README.md" "1" "$?"

# --- Test 8b: zip mode, archive missing test/Makefile ---
mkdir -p "$WORK/bad_zip_test/staging/docs" \
         "$WORK/bad_zip_test/staging/test/test_desktop" \
         "$WORK/bad_zip_test/staging/test/common" \
         "$WORK/bad_zip_test/staging/test/scripts/coverage"
: > "$WORK/bad_zip_test/staging/AGENTS.md"
: > "$WORK/bad_zip_test/staging/README.md"
: > "$WORK/bad_zip_test/staging/docs/Version.md"
: > "$WORK/bad_zip_test/staging/test/test_desktop/Makefile"
: > "$WORK/bad_zip_test/staging/test/common/MockHal.h"
: > "$WORK/bad_zip_test/staging/test/scripts/coverage/coverage_manifest.py"
# Missing test/Makefile
(cd "$WORK/bad_zip_test/staging" && zip -qr "$WORK/bad_test.zip" .)
"$GATE" --zip "$WORK/bad_test.zip" > /dev/null 2>&1
assert_exit "zip mode, missing test/Makefile" "1" "$?"

# --- Test 9: zip mode, archive with wrapper folder (no
#     AGENTS.md at root) ---
mkdir -p "$WORK/wrap/staging/docs"
: > "$WORK/wrap/staging/AGENTS.md"
: > "$WORK/wrap/staging/README.md"
: > "$WORK/wrap/staging/docs/Version.md"
# Zip from parent (not staging/), so the archive has a
# wrapper folder 'staging/'.
(cd "$WORK/wrap" && zip -qr "$WORK/wrap.zip" staging)
"$GATE" --zip "$WORK/wrap.zip" > /dev/null 2>&1
assert_exit "zip mode, wrapper folder (no AGENTS.md at root)" "1" "$?"

# --- Test 10: zip mode, non-existent file ---
"$GATE" --zip "$WORK/no_such_zip_here.zip" > /dev/null 2>&1
assert_exit "zip mode, non-existent file" "1" "$?"

# --- Test 11: directory mode, non-existent path ---
"$GATE" "$WORK/no_such_dir" > /dev/null 2>&1
assert_exit "dir mode, non-existent path" "1" "$?"

# --- Test 12 (AL90-14): zip mode, archive with
#     run_test_* binary (stale-binary slip) ---
mkdir -p "$WORK/stale/staging/docs" \
         "$WORK/stale/staging/test/test_desktop" \
         "$WORK/stale/staging/test/common" \
         "$WORK/stale/staging/test/scripts/coverage"
: > "$WORK/stale/staging/AGENTS.md"
: > "$WORK/stale/staging/README.md"
: > "$WORK/stale/staging/docs/Version.md"
: > "$WORK/stale/staging/test/Makefile"
: > "$WORK/stale/staging/test/test_desktop/Makefile"
: > "$WORK/stale/staging/test/test_desktop/run_test_field_wedge_fixes_89"
: > "$WORK/stale/staging/test/common/MockHal.h"
: > "$WORK/stale/staging/test/scripts/coverage/coverage_manifest.py"
(cd "$WORK/stale/staging" && zip -qr "$WORK/stale.zip" .)
"$GATE" --zip "$WORK/stale.zip" > /dev/null 2>&1
assert_exit "zip mode, stale run_test_* binary" "1" "$?"

# --- Test 13 (AL90-14): zip mode, archive with
#     *.o object file ---
mkdir -p "$WORK/obj/staging/docs" \
         "$WORK/obj/staging/test/test_desktop" \
         "$WORK/obj/staging/test/common" \
         "$WORK/obj/staging/test/scripts/coverage"
: > "$WORK/obj/staging/AGENTS.md"
: > "$WORK/obj/staging/README.md"
: > "$WORK/obj/staging/docs/Version.md"
: > "$WORK/obj/staging/test/Makefile"
: > "$WORK/obj/staging/test/test_desktop/Makefile"
: > "$WORK/obj/staging/test/test_desktop/LinkCore.o"
: > "$WORK/obj/staging/test/common/MockHal.h"
: > "$WORK/obj/staging/test/scripts/coverage/coverage_manifest.py"
(cd "$WORK/obj/staging" && zip -qr "$WORK/obj.zip" .)
"$GATE" --zip "$WORK/obj.zip" > /dev/null 2>&1
assert_exit "zip mode, stale .o object" "1" "$?"

# --- Test 14 (AL90-14): directory mode, staging
#     root carrying run_test_* binaries ---
mkdir -p "$WORK/dirbin/docs" \
         "$WORK/dirbin/test/test_desktop" \
         "$WORK/dirbin/test/common" \
         "$WORK/dirbin/test/scripts/coverage"
: > "$WORK/dirbin/AGENTS.md" \
  "$WORK/dirbin/README.md" \
  "$WORK/dirbin/docs/Version.md" \
  "$WORK/dirbin/test/Makefile" \
  "$WORK/dirbin/test/test_desktop/Makefile" \
  "$WORK/dirbin/test/test_desktop/run_test_some_suite" \
  "$WORK/dirbin/test/common/MockHal.h" \
  "$WORK/dirbin/test/scripts/coverage/coverage_manifest.py"
"$GATE" "$WORK/dirbin" > /dev/null 2>&1
assert_exit "dir mode, stale test binary" "1" "$?"

# --- Test 15 (AL92-8): directory mode, staging
#     root with NO DOT ANYWHERE IN THE PATH.
#     Test 14 above roots itself under $WORK,
#     which is created by `mktemp -d -t
#     pre_zip_check_test.XXXXXX` and so always
#     contains a literal dot upstream in the path
#     — that dot let the extensionless check's old
#     form ("${f##*.}" = "$(basename "$f")",
#     comparing a full-PATH strip against a bare
#     basename) coincidentally behave as if it
#     worked, while silently passing on a fully
#     dotless root (verified by hand: `bash
#     pre_zip_check.sh /tmp/dirbin` returned OK on
#     a staging tree carrying a stale
#     run_test_some_suite binary). Root this case
#     directly under /tmp with a dotless name so a
#     regression back to the old comparison form
#     fails here regardless of where $TMPDIR
#     happens to put mktemp's own directories.
DOTLESS="/tmp/pre_zip_check_test_dotless_$$"
rm -rf "$DOTLESS"
mkdir -p "$DOTLESS/docs" \
         "$DOTLESS/test/test_desktop" \
         "$DOTLESS/test/common" \
         "$DOTLESS/test/scripts/coverage"
: > "$DOTLESS/AGENTS.md" \
  "$DOTLESS/README.md" \
  "$DOTLESS/docs/Version.md" \
  "$DOTLESS/test/Makefile" \
  "$DOTLESS/test/test_desktop/Makefile" \
  "$DOTLESS/test/test_desktop/run_test_some_suite" \
  "$DOTLESS/test/common/MockHal.h" \
  "$DOTLESS/test/scripts/coverage/coverage_manifest.py"
"$GATE" "$DOTLESS" > /dev/null 2>&1
assert_exit "dir mode, dotless staging root, stale test binary" "1" "$?"
rm -rf "$DOTLESS"

# --- Test 17-20 (AL-A1 / AL-A4): --require-crosscompile.
#     A complete, clean root (reuse $ROOT from the earlier complete-
#     root case) with no build/verify_build/.last-pass must fail;
#     with --allow-unverified must pass (with a warning on stderr);
#     with a stamp OLDER than a src/ file must fail; with a stamp
#     NEWER than every src/ file must pass.
CCROOT="$WORK/ccroot"
rm -rf "$CCROOT"
mkdir -p "$CCROOT/docs" "$CCROOT/src" "$CCROOT/include" "$CCROOT/build" \
         "$CCROOT/test/test_desktop" "$CCROOT/test/common" \
         "$CCROOT/test/scripts/coverage"
touch "$CCROOT/AGENTS.md" "$CCROOT/README.md" \
  "$CCROOT/test/Makefile" "$CCROOT/test/test_desktop/Makefile" \
  "$CCROOT/test/common/MockHal.h" \
  "$CCROOT/test/scripts/coverage/coverage_manifest.py" \
  "$CCROOT/src/AutoLink.cpp" "$CCROOT/include/AutoLink.h"
: > "$CCROOT/build/rule20a_baseline.txt"
printf '%s' "$VERSION_MD_STUB" > "$CCROOT/docs/Version.md"

"$GATE" "$CCROOT" --require-crosscompile > /dev/null 2>&1
assert_exit "require-crosscompile, no stamp" "1" "$?"

"$GATE" "$CCROOT" --require-crosscompile --allow-unverified \
    > /tmp/pre_zip_check_test_warn.$$ 2>&1
rc=$?
assert_exit "require-crosscompile + allow-unverified" "0" "$rc"
if ! grep -q "WARNING" /tmp/pre_zip_check_test_warn.$$; then
    echo "  FAIL: require-crosscompile + allow-unverified — expected a WARNING line on stderr"
    fail=$((fail + 1))
else
    pass=$((pass + 1))
fi
rm -f /tmp/pre_zip_check_test_warn.$$

mkdir -p "$CCROOT/build/verify_build"
: > "$CCROOT/build/verify_build/.last-pass"
touch -d "2000-01-01" "$CCROOT/build/verify_build/.last-pass"
"$GATE" "$CCROOT" --require-crosscompile > /dev/null 2>&1
assert_exit "require-crosscompile, stale stamp" "1" "$?"

touch "$CCROOT/build/verify_build/.last-pass"
sleep 1
"$GATE" "$CCROOT" --require-crosscompile > /dev/null 2>&1
assert_exit "require-crosscompile, fresh stamp" "0" "$?"
rm -rf "$CCROOT"

# --- Test 21-22 (AL-C1): version.py check wired into pre_zip_check.
#     A staging root whose docs/Version.md is over DEFAULT_KEEP
#     entries must fail; one at or under the cap must pass. Build
#     the file directly rather than depending on the real
#     docs/Version.md's entry count, which changes every release.
VERROOT="$WORK/verroot"
rm -rf "$VERROOT"
mkdir -p "$VERROOT/docs" "$VERROOT/test/test_desktop" \
         "$VERROOT/test/common" "$VERROOT/test/scripts/coverage"
touch "$VERROOT/AGENTS.md" "$VERROOT/README.md" \
      "$VERROOT/test/Makefile" "$VERROOT/test/test_desktop/Makefile" \
      "$VERROOT/test/common/MockHal.h" \
      "$VERROOT/test/scripts/coverage/coverage_manifest.py"
{
    printf '# 📅 AutoLink Version History\n\nAll releases, most recent first.\n'
    for i in $(seq 25 -1 1); do
        printf '## v0.0.%d\n\nentry %d\n\n' "$i" "$i"
    done
} > "$VERROOT/docs/Version.md"
"$GATE" "$VERROOT" > /dev/null 2>&1
assert_exit "version.py check, over DEFAULT_KEEP" "1" "$?"

: > "$VERROOT/docs/Version.md"
printf '# 📅 AutoLink Version History\n\nAll releases, most recent first.\n## v0.0.1\n\nentry\n\n' > "$VERROOT/docs/Version.md"
"$GATE" "$VERROOT" > /dev/null 2>&1
assert_exit "version.py check, under DEFAULT_KEEP" "0" "$?"
rm -rf "$VERROOT"

# --- Test 23-24 (AL-E1): rule20a_check wired into pre_zip_check.
#     A staging root with a bare, empty build/rule20a_baseline.txt
#     and a >15 KB source file must fail (a NEW violation, nothing
#     in the baseline to cover it); the same file listed in the
#     baseline must pass.
E1ROOT="$WORK/e1root"
rm -rf "$E1ROOT"
mkdir -p "$E1ROOT/docs" "$E1ROOT/src/al" "$E1ROOT/include" "$E1ROOT/build" \
         "$E1ROOT/test/test_desktop" "$E1ROOT/test/common" \
         "$E1ROOT/test/scripts/coverage"
touch "$E1ROOT/AGENTS.md" "$E1ROOT/README.md" \
      "$E1ROOT/test/Makefile" "$E1ROOT/test/test_desktop/Makefile" \
      "$E1ROOT/test/common/MockHal.h" \
      "$E1ROOT/test/scripts/coverage/coverage_manifest.py"
printf '# 📅 AutoLink Version History\n\nAll releases, most recent first.\n## v0.0.1\n\nentry\n\n' \
    > "$E1ROOT/docs/Version.md"
: > "$E1ROOT/build/rule20a_baseline.txt"
python3 -c "print('// x' * 5000)" > "$E1ROOT/src/al/oversized.h"

"$GATE" "$E1ROOT" > /dev/null 2>&1
assert_exit "rule20a_check, new violation, empty baseline" "1" "$?"

echo "src/al/oversized.h" > "$E1ROOT/build/rule20a_baseline.txt"
"$GATE" "$E1ROOT" > /dev/null 2>&1
assert_exit "rule20a_check, violation covered by baseline" "0" "$?"
rm -rf "$E1ROOT"

echo
echo "=== pre_zip_check self-test: $pass pass, $fail fail ==="
if [ $fail -gt 0 ]; then
    exit 1
fi
exit 0
