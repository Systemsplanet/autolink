#!/usr/bin/env bash
# AL-E1: AGENTS.md rule 20a caps directories at 7 files and source
# files at 15 KB. Measured against the actual tree: 3 directories
# and 42 files violate it, none of it enforced anywhere — an
# unenforced size rule that the whole codebase violates trains
# everyone to ignore the rulebook, which is worse than not having
# the rule.
#
# The two honest options were "enforce it" or "delete it." Deleting
# it throws away a real signal (a 62 KB test file, a 53 KB source
# file, are genuinely harder to review and more likely to hide a
# regression — see this project's own history of a 12-pin
# source-grep suite missing a real throughput regression). Enforcing
# it by splitting all 45 existing violations in one pass is a mass
# refactor across the whole codebase with real risk of breaking
# includes, build rules, and Makefile targets for no behavioral
# benefit — not something to do blind in one sitting.
#
# So: ratchet, same shape as AL-C3's dangling-pin baseline.
# build/rule20a_baseline.txt is the exact list of pre-existing
# violations. Anything violating the cap that ISN'T in the baseline
# is NEW — introduced after this fix — and fails. Anything in the
# baseline is disclosed, existing debt; it's still reported so it
# stays visible, but doesn't block. Paying down the baseline is:
# split the file (or thin the directory), remove its line from
# rule20a_baseline.txt, done.
set -uo pipefail
# AL-E1 usage: bash build/rule20a_check.sh [root]
# root defaults to the project root two levels up from this
# script's own location (build/) — the shape used when run
# directly, e.g. `bash build/rule20a_check.sh` from a checkout.
# pre_zip_check.sh passes an explicit root (a staging directory or
# a temp-extracted zip) so this checks THAT tree, not the tree
# rule20a_check.sh itself happens to live in.
root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$root"

BASELINE="build/rule20a_baseline.txt"
if [ ! -f "$BASELINE" ]; then
    echo "rule20a_check: $BASELINE not found" >&2
    exit 2
fi

current=()

while IFS= read -r d; do
    n=$(find "$d" -maxdepth 1 -type f 2>/dev/null | wc -l)
    if [ "$n" -gt 7 ]; then
        current+=("$d")
    fi
done < <(find src test -type d ! -path "*/node_modules/*" 2>/dev/null)

while IFS= read -r f; do
    sz=$(stat -c %s "$f" 2>/dev/null || echo 0)
    if [ "$sz" -gt 15360 ]; then
        current+=("$f")
    fi
done < <(find src test \( -name '*.cpp' -o -name '*.h' \) 2>/dev/null)

new=()
for c in "${current[@]}"; do
    if ! grep -Fxq "$c" "$BASELINE"; then
        new+=("$c")
    fi
done

if [ ${#current[@]} -gt 0 ]; then
    echo "rule20a_check: ${#current[@]} entries over the AGENTS.md rule 20a" \
         "cap (7 files/dir, 15 KB/file) — $(( ${#current[@]} - ${#new[@]} ))" \
         "pre-existing (see $BASELINE), ${#new[@]} NEW"
fi

if [ ${#new[@]} -gt 0 ]; then
    echo "" >&2
    echo "rule20a_check: FAILED — new rule 20a violation(s) not in the" \
         "disclosed baseline:" >&2
    for n in "${new[@]}"; do
        echo "  $n" >&2
    done
    echo "Either split the file / thin the directory, or — if this is" \
         "pre-existing debt being surfaced by an unrelated change — add" \
         "it to $BASELINE explicitly." >&2
    exit 1
fi

echo "rule20a_check: no NEW rule 20a violations"
