#!/usr/bin/env python3
# Generate a coverage manifest from the test Makefile.
#
# coverage_merge.sh used to maintain a hardcoded `src_for` map listing
# which test binaries link each library source. Whenever a new suite
# was added to TEST_BINS, that map had to be hand-edited or coverage
# silently missed the new suite (see AGENTS.md rule 4).
#
# This script is the single source of truth instead. It scans the
# Makefile, expands `$(VAR)` references using the Makefile's own
# variable definitions, and emits a shell-sourceable file mapping
# each library source basename to the list of run_test_* binaries
# whose build rule references it.
#
# Usage:
#   coverage_manifest.py <Makefile> <TEST_BINS_LIST_FILE> <out_manifest>
#
#   <Makefile>             path to the test Makefile
#   <TEST_BINS_LIST_FILE>  path to a file containing the canonical
#                          TEST_BINS list, one binary per line
#   <out_manifest>         path to write the shell-sourceable manifest;
#                          emits lines of the form
#                            src_for_<basename>="<bin1> <bin2> ..."
#                          plus a
#                            TEST_BINS="<bin1> <bin2> ..."
#                          that the calling script can source.
#
# Library sources are recognised by path: anything rooted under
# ../../src/ or one of the lib make-variables (SRC, AL, PROTO, HAL,
# UTIL, WEB, PONG, LINK_SRC, AUTOLINK_SRC). Test sources under al/,
# hal/, util/, web/, pingpong/ are filtered out — coverage_merge.sh
# handles those via a separate per-test-cpp branch.

import os
import re
import sys


# Test-tree prefixes mark a path as a test cpp/h, which the merge
# script handles separately. Anything NOT in this set (and matching
# one of the lib-root prefixes below) is treated as a library source.
TEST_TREE_PREFIXES = ('al/', '$(TAL)', '$(THAL)', '$(TPROTO)',
                      '$(TUTIL)', '$(TWEB)', '$(TPING)',
                      '../common', '$(COMMON)')

# Library source roots. A path whose textual form starts with any of
# these (or matches the bare $(VAR) reference) is a library source.
LIB_PREFIXES = ('../../src/', '$(SRC)/', '$(AL)/', '$(PROTO)/',
                '$(HAL)/', '$(UTIL)/', '$(WEB)/', '$(PONG)/',
                '$(LINK_SRC)', '$(AUTOLINK_SRC)')


def is_library_source(path):
    for pref in LIB_PREFIXES:
        if path.startswith(pref):
            return True
    return False


def is_test_source(path):
    for pref in TEST_TREE_PREFIXES:
        if path.startswith(pref):
            return True
    return False


def parse_makefile_vars(text):
    # `NAME = VALUE` and `NAME := VALUE`, joining backslash
    # continuations. References like `$(OTHER)` are kept verbatim and
    # expanded lazily in expand().
    var_re = re.compile(r'^([A-Za-z_][A-Za-z0-9_]*)\s*[:?]?=\s*(.*)$')
    cont_re = re.compile(r'\\\s*\n')
    flat = cont_re.sub(' ', text)
    vars_ = {}
    for line in flat.splitlines():
        hash_idx = line.find(' #')
        if hash_idx >= 0 and '$(' not in line[:hash_idx]:
            line = line[:hash_idx].rstrip()
        m = var_re.match(line)
        if not m:
            continue
        vars_[m.group(1)] = m.group(2).strip()
    return vars_


def expand(text, vars_, seen=None):
    # Recursive $(NAME) expansion. Loops until stable so chained refs
    # (`A = $(B)`, `B = foo.cpp`) resolve fully. Cycles fall back to
    # the empty string, matching make's behaviour for self-references.
    if seen is None:
        seen = set()
    pat = re.compile(r'\$\(([A-Za-z_][A-Za-z0-9_]*)\)')

    def repl(m):
        name = m.group(1)
        if name in seen:
            return ''
        return expand(vars_.get(name, m.group(0)), vars_, seen | {name})

    prev = None
    out = text
    while out != prev:
        prev = out
        out = pat.sub(repl, out)
    return out


def parse_run_test_rules(text):
    # Yield (binary_name, joined_body) for each `run_test_X:` rule,
    # joining the prereq line with the recipe body (which may span
    # multiple lines). Backslash continuations are flattened first.
    cont_re = re.compile(r'\\\s*\n')
    flat = cont_re.sub(' ', text)
    rule_re = re.compile(
        r'^(run_test_[A-Za-z0-9_]+)\s*:[ \t]*(.*?)\n((?:[ \t]+.*\n?)*)',
        re.MULTILINE)
    rules = []
    for m in rule_re.finditer(flat):
        bin_name = m.group(1)
        prereq = m.group(2).strip()
        recipe = m.group(3).replace('\n', ' ').strip()
        body = ' '.join(filter(None, [prereq, recipe]))
        rules.append((bin_name, body))
    return rules


def main():
    if len(sys.argv) != 4:
        sys.stderr.write(
            "usage: coverage_manifest.py <Makefile> "
            "<TEST_BINS_LIST_FILE> <out_manifest>\n")
        sys.exit(2)

    makefile_path, test_bins_path, out_path = sys.argv[1:4]
    with open(makefile_path) as f:
        makefile_text = f.read()
    with open(test_bins_path) as f:
        test_bins = [ln.strip() for ln in f if ln.strip()]

    vars_ = parse_makefile_vars(makefile_text)
    rules_by_bin = dict(parse_run_test_rules(makefile_text))

    src_for = {}  # basename -> set of test bins
    for bin_name in test_bins:
        body = rules_by_bin.get(bin_name)
        if body is None:
            # No rule found; the binary may be a leaf phony or a
            # sub-target that doesn't link a library source. Skip
            # silently — coverage still works, this binary just
            # won't contribute to any src_for_<basename>.
            continue
        expanded = expand(body, vars_)
        # Both raw and expanded paths: raw preserves the
        # $(VAR)/../../src/ prefix that the lib/test classifiers
        # rely on; expanded reveals the actual .cpp basenames.
        all_paths = (set(re.findall(r'\S+\.(?:cpp|h)\b', body)) |
                     set(re.findall(r'\S+\.(?:cpp|h)\b', expanded)))

        for p in all_paths:
            if is_test_source(p) and not is_library_source(p):
                continue
            if not is_library_source(p):
                continue
            base = os.path.basename(p)
            base = re.sub(r'\.(cpp|h)$', '', base)
            if not base:
                continue
            src_for.setdefault(base, set()).add(bin_name)

    with open(out_path, 'w') as f:
        f.write('# Auto-generated by coverage_manifest.py — do not edit.\n')
        f.write('# Source of truth: TEST_BINS and the per-suite build\n')
        f.write('# rules in the test Makefile. Re-run this generator\n')
        f.write('# whenever TEST_BINS or a build rule changes.\n\n')
        f.write('TEST_BINS="%s"\n' % ' '.join(test_bins))
        for base in sorted(src_for):
            f.write('src_for_%s="%s"\n' % (base,
                                           ' '.join(sorted(src_for[base]))))


if __name__ == '__main__':
    main()
