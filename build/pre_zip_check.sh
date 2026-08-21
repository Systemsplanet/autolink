#!/usr/bin/env bash
# Pre-zip gate: hard check that the deliverable
# (zip or staging root) carries the three files
# every release needs (AGENTS.md / README.md /
# docs/Version.md). A non-empty
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

# AL-A1: block a release that has no evidence a cross-compile ever
# passed against it. build/verify_build.sh stamps
# build/verify_build/.last-pass (AL-A4) on a real pass; this flag
# refuses to proceed unless that stamp exists and is newer than
# every file under src/ and include/ — i.e. the pass was against
# THIS tree, not a stale one from before the last source edit. The
# environment this project has shipped from for months cannot run
# arduino-cli at all (no network egress to install it — see
# docs/Version.md, AL-A1's own analysis) — AGENTS.md rule 4 called
# the cross-compile "not optional on any change" and then, in the
# next sentence, said to disclose and ship anyway when it can't run.
# That escape hatch is what let a real dram0_0_seg overflow ship
# (see docs/Version.md). --allow-unverified is the same escape
# because it is sometimes genuinely necessary (this environment),
# but it is no longer silent: it is a flag the caller must pass
# explicitly, and pre_zip_check prints a loud warning either way so
# it shows up in the delivery transcript rather than only in
# docs/Version.md prose.
require_crosscompile=0
allow_unverified=0
args=()
for a in "$@"; do
    case "$a" in
        --require-crosscompile) require_crosscompile=1 ;;
        --allow-unverified) allow_unverified=1 ;;
        *) args+=("$a") ;;
    esac
done
set -- "${args[@]+"${args[@]}"}"

# AL-C1: build/version.py check was written and documented (AGENTS
# rule 5) as "the pre-zip gate" but nothing ever called it — a
# docs/Version.md over its own DEFAULT_KEEP cap (build/version.py)
# could ship indefinitely. version.py is a sibling of this script.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
check_version_md() {
    # $1 = path to a Version.md file on disk (staging root's, or a
    # temp-extracted copy from a zip). --path is a top-level flag on
    # version.py (defined before the add_subparsers() call) and
    # argparse requires it before the subcommand name, not after.
    python3 "$SCRIPT_DIR/version.py" --path "$1" check
}

# AL-E1: rule 20a (file/dir size cap) as a non-regression ratchet —
# see build/rule20a_check.sh's own header. $1 = a root that has
# src/ and test/ under it (a staging directory, or a temp-extracted
# zip). Only meaningful when the root actually carries source (a
# zip missing src/ entirely — which the required-entries check
# above would already have failed on — would otherwise report
# false negatives here).
check_rule20a() {
    local root="$1"
    if [ ! -d "$root/src" ] || [ ! -d "$root/test" ]; then
        return 0
    fi
    bash "$SCRIPT_DIR/rule20a_check.sh" "$root"
}

check_crosscompile_stamp() {
    # $1 = root to check (a staging directory, or a temp dir the
    # zip was extracted into).
    local root="$1"
    local stamp="$root/build/verify_build/.last-pass"
    if [ "$allow_unverified" -eq 1 ]; then
        echo "pre_zip_check: WARNING — shipping with --allow-unverified;" >&2
        echo "  no cross-compile was confirmed against this tree." >&2
        return 0
    fi
    if [ ! -f "$stamp" ]; then
        echo "pre_zip_check: --require-crosscompile set, but no" >&2
        echo "  $stamp exists. Run build/verify_build.sh, or pass" >&2
        echo "  --allow-unverified if a cross-compile genuinely" >&2
        echo "  cannot run in this environment." >&2
        return 1
    fi
    # Newer than every src/ and include/ file? find -newer is a
    # single-file comparison; loop until one is newer than the
    # stamp, which means the stamp predates a source edit.
    local stale
    stale=$(find "$root/src" "$root/include" -type f -newer "$stamp" 2>/dev/null | head -1)
    if [ -n "$stale" ]; then
        echo "pre_zip_check: --require-crosscompile set, but" >&2
        echo "  $stamp is older than $stale —" >&2
        echo "  the cross-compile pass predates a source change." >&2
        echo "  Re-run build/verify_build.sh, or pass" >&2
        echo "  --allow-unverified if it genuinely cannot run here." >&2
        return 1
    fi
    echo "pre_zip_check: cross-compile stamp OK ($stamp, newer than every src/include file)"
    return 0
}

required=(AGENTS.md README.md docs/Version.md
          test/Makefile test/test_desktop/Makefile
          test/common/MockHal.h test/scripts/coverage/coverage_manifest.py)

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
    # AL90-14: strip-list assertion.
    # AGENTS rule 8 forbids shipping build
    # artifacts. The pre-zip gate must catch
    # a stale-binary slip (the prior
    # release's "every required entry
    # present" pass was green on a zip
    # carrying 140 run_test_* binaries).
    # Reject the zip on any *.o, *.bak,
    # *.gcno, *.gcda, compile_commands.json,
    # node_modules/ entry, build cache, or
    # extensionless test binary.
    # Pinned by StripListEnforcedTest.
    bad_strip=()
    while IFS= read -r entry; do
        # Skip directory entries
        # (unzip -l prints them with a
        # trailing /).
        case "$entry" in
            */) continue ;;
        esac
        case "$entry" in
            *.o|*.bak|*.gcno|*.gcda)
                bad_strip+=("$entry") ;;
            */compile_commands.json)
                bad_strip+=("$entry") ;;
            */node_modules/*)
                bad_strip+=("$entry") ;;
            build/verify_build/build/*|build/verify_build/libraries/*)
                bad_strip+=("$entry") ;;
        esac
        # Extensionless entry under a
        # test_desktop/ tree that isn't a
        # Makefile or package metadata =
        # test binary. A prior release
        # shipped 140 of these.
        case "$entry" in
            test/test_desktop/*|test/itest/test_desktop/*)
                base=$(basename "$entry")
                if [ "${base##*.}" = "$base" ] && \
                   [ "$base" != "Makefile" ] && \
                   [ "$base" != "package.json" ] && \
                   [ "$base" != "manifest.json" ]; then
                    bad_strip+=("$entry")
                fi
                ;;
        esac
    done < <(printf '%s\n' "$list")
    if [ ${#bad_strip[@]} -gt 0 ]; then
        echo "pre_zip_check: $zip_path carries build artifacts:" >&2
        for f in "${bad_strip[@]}"; do
            echo "  $f" >&2
        done
        echo "AGENTS.md rule 8: build/verify_build caches, *.o, *.bak, " >&2
        echo "extensionless test binaries, and compile_commands.json " >&2
        echo "must not ship in a release zip. Run \`build/clean.py " >&2
        echo "--apply\` on the staging root before zipping." >&2
        exit 1
    fi
    echo "pre_zip_check: $zip_path OK (${#required[@]} required entries present)"
    tmp_vermd=$(mktemp -d)
    if unzip -p "$zip_path" docs/Version.md > "$tmp_vermd/Version.md" 2>/dev/null; then
        if ! check_version_md "$tmp_vermd/Version.md"; then
            rm -rf "$tmp_vermd"
            exit 1
        fi
    fi
    rm -rf "$tmp_vermd"
    tmp_rule20a=$(mktemp -d)
    unzip -q "$zip_path" -d "$tmp_rule20a"
    if ! check_rule20a "$tmp_rule20a"; then
        rm -rf "$tmp_rule20a"
        exit 1
    fi
    rm -rf "$tmp_rule20a"
    if [ "$require_crosscompile" -eq 1 ]; then
        tmpd=$(mktemp -d)
        unzip -q "$zip_path" -d "$tmpd"
        if ! check_crosscompile_stamp "$tmpd"; then
            rm -rf "$tmpd"
            exit 1
        fi
        rm -rf "$tmpd"
    fi
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
# AL90-14: staging-root strip-list
# assertion. Same checks as the zip
# mode, against the on-disk staging
# tree. Pinned by StripListEnforcedTest.
bad_strip=()
if [ -d "$root/test/test_desktop" ]; then
    while IFS= read -r f; do
        case "$(basename "$f")" in
            Makefile|package.json|manifest.json) ;;
            *)
                # AL92-8: compare the STRIPPED
                # BASENAME against the basename,
                # not the stripped full PATH
                # against the basename. The
                # previous form ("${f##*.}" =
                # "$(basename "$f")") strips at
                # the last dot anywhere in the
                # whole path; for a dotless
                # staging root (e.g.
                # /tmp/dirbin/test/test_desktop/
                # run_test_foo) no dot exists
                # anywhere, "##*." matches
                # nothing, and ${f##*.} returns
                # the untouched FULL PATH — which
                # never equals a bare basename,
                # so the check silently passed
                # every extensionless file. The
                # zip-mode branch above already
                # gets this right (basename
                # first, then strip); mirror it
                # here. AL92-9 also drops the
                # dead "! -s .$RANDOM_marker"
                # condition, which tested a
                # randomly-named file that can
                # never exist and was always
                # true.
                base=$(basename "$f")
                if [ -f "$f" ] && [ "${base##*.}" = "$base" ]; then
                    bad_strip+=("$f")
                fi
                ;;
        esac
    done < <(find "$root/test/test_desktop" -maxdepth 4 -type f ! -name '*.cpp' ! -name '*.h' ! -name '*.md')
fi
if [ ${#bad_strip[@]} -gt 0 ]; then
    echo "pre_zip_check: $root carries test binaries:" >&2
    for f in "${bad_strip[@]}"; do
        echo "  $f" >&2
    done
    echo "AGENTS.md rule 8: run \`build/clean.py --apply\` first." >&2
    exit 1
fi
# Also reject *.o / *.bak / *.gcno / *.gcda /
# compile_commands.json / build caches under
# the staging root.
if [ -d "$root" ]; then
    bad_strip_root=()
    while IFS= read -r f; do
        bad_strip_root+=("$f")
    done < <(find "$root" \( \
        -name '*.o' -o \
        -name '*.bak' -o \
        -name '*.gcno' -o \
        -name '*.gcda' -o \
        -name 'compile_commands.json' \
        \) 2>/dev/null)
    while IFS= read -r d; do
        bad_strip_root+=("$d")
    done < <(find "$root" \( \
        -path '*/build/verify_build/build' -o \
        -path '*/build/verify_build/libraries' -o \
        -path '*/node_modules' \
        \) -type d 2>/dev/null)
    if [ ${#bad_strip_root[@]} -gt 0 ]; then
        echo "pre_zip_check: $root carries build artifacts:" >&2
        for f in "${bad_strip_root[@]}"; do
            echo "  $f" >&2
        done
        echo "AGENTS.md rule 8: run \`build/clean.py --apply\` first." >&2
        exit 1
    fi
fi
echo "pre_zip_check: $root OK (${#required[@]} required files present)"
if [ -f "$root/docs/Version.md" ]; then
    if ! check_version_md "$root/docs/Version.md"; then
        exit 1
    fi
fi
if ! check_rule20a "$root"; then
    exit 1
fi
if [ "$require_crosscompile" -eq 1 ]; then
    if ! check_crosscompile_stamp "$root"; then
        exit 1
    fi
fi
exit 0
