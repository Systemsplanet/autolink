#!/usr/bin/env python3
# Self-test for coverage_manifest.py.
#
# Pins AGENTS.md rule 4 (coverage_merge.sh and TEST_BINS drift
# independently). The bug was: when a new suite was added to
# TEST_BINS, the hardcoded `src_for` map in coverage_merge.sh had
# to be hand-edited or coverage silently missed the new suite.
#
# The fix derives the map from the Makefile's own per-suite build
# rules, so adding a suite to TEST_BINS automatically extends
# coverage. This test simulates that addition and asserts the new
# suite shows up in every src_for_<basename> entry it should.
#
# Two layers of pinning:
#   1. Sanity: the manifest produced from the real Makefile covers
#      every test binary currently in TEST_BINS. If a builder
#      drops a binary from TEST_BINS without updating the rules,
#      this fails.
#   2. Drift: feeding the manifest generator a synthetic Makefile
#      with a new run_test_* rule that links $(AUTOLINK_SRC) makes
#      the new binary appear in src_for_AutoLink, src_for_Link,
#      src_for_Log, src_for_UtilCrc, src_for_UtilCobs, etc. — the
#      same set of sources the regression would otherwise miss.
#      This is the test that fails when the fix is reverted (e.g.
#      if someone replaces the manifest generator with a static
#      map again).

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GENERATOR = os.path.join(HERE, 'coverage_manifest.py')
MAKEFILE = os.path.join(HERE, 'Makefile')


def run_generator(makefile_text, test_bins):
    with tempfile.TemporaryDirectory() as tmp:
        mk = os.path.join(tmp, 'Makefile')
        bins_list = os.path.join(tmp, 'bins.list')
        out = os.path.join(tmp, 'manifest.sh')
        with open(mk, 'w') as f:
            f.write(makefile_text)
        with open(bins_list, 'w') as f:
            for b in test_bins:
                f.write(b + '\n')
        r = subprocess.run(
            [sys.executable, GENERATOR, mk, bins_list, out],
            capture_output=True, text=True)
        if r.returncode != 0:
            print('generator failed:', r.stderr)
            sys.exit(1)
        with open(out) as f:
            text = f.read()
    return text


def test_real_makefile_covers_every_test_bin():
    # Sanity: every binary in TEST_BINS should appear in the
    # manifest's TEST_BINS echo and contribute to at least one
    # src_for_<basename> entry (a suite that links nothing would
    # be a bug — the unit suite has no such case).
    with open(MAKEFILE) as f:
        mk_text = f.read()
    # TEST_BINS spans multiple lines via backslash continuations;
    # flatten those before parsing the value.
    flat = re.sub(r'\\\s*\n', ' ', mk_text)
    bins = re.findall(r'run_test_[A-Za-z0-9_]+', mk_text)
    # Dedup, keep order, drop anything we know isn't a TEST_BINS
    # entry by reading the actual TEST_BINS line.
    bins_var = re.search(r'^TEST_BINS\s*=\s*(.*?)$', flat, re.MULTILINE)
    assert bins_var, 'TEST_BINS line not found'
    test_bins = bins_var.group(1).split()
    missing = [b for b in test_bins if b not in bins]
    assert not missing, 'TEST_BINS lists %s but no rule found' % missing

    manifest = run_generator(mk_text, test_bins)
    m_bins_match = re.search(r'^TEST_BINS="([^"]*)"', manifest, re.MULTILINE)
    assert m_bins_match, 'manifest has no TEST_BINS echo'
    m_bins = m_bins_match.group(1).split()
    assert sorted(m_bins) == sorted(test_bins), \
        'manifest TEST_BINS %s != Makefile TEST_BINS %s' % (
            sorted(m_bins), sorted(test_bins))

    # Every binary in TEST_BINS that links $(AUTOLINK_SRC) or
    # $(LINK_SRC) should appear in src_for_Link (sanity: a
    # binary that does appear in TEST_BINS but is not reflected
    # in any src_for entry indicates a Makefile rule the manifest
    # generator failed to parse).
    src_for = re.findall(r'^src_for_(\w+)="([^"]*)"', manifest, re.MULTILINE)
    bin_to_srcs = {}
    for base, bins_str in src_for:
        for b in bins_str.split():
            bin_to_srcs.setdefault(b, set()).add(base)
    for b in test_bins:
        if b in ('run_test_blink', 'run_test_compile_check',
                 'run_test_esp_idf_error_etiquette',
                 'run_test_ping_resume_source',
                 'run_test_pingpong_structure',
                 'run_test_linkdecision',
                 'run_test_version_free_source'):
            # These suites link a single utility / a test cpp
            # only, no library source. They have no src_for entry
            # by design.
            continue
        assert b in bin_to_srcs, (
            '%s is in TEST_BINS but contributes to no '
            'src_for_<basename> entry — the manifest generator '
            'failed to parse its Makefile rule. This is exactly '
            'the drift bug AGENTS.md rule 4 warns about.' % b)


def test_new_suite_picked_up_automatically():
    # Regression for AGENTS.md rule 4. Synthesise a Makefile that
    # mimics the real one (with the same $(LINK_SRC) / $(AUTOLINK_SRC)
    # definitions and a fake run_test_zzz that links them), feed
    # it to the generator, and assert the new suite appears in
    # every src_for_<basename> entry it should.
    fake_mk = '''\
SRC      = ../../src
AL       = $(SRC)/al
PROTO    = $(AL)/link
HAL      = $(AL)/hal
UTIL     = $(AL)/util
WEB      = $(AL)/web
PONG     = $(AL)/pingpong
TAL      = al
THAL     = $(TAL)/hal
TPROTO   = $(TAL)/link
TUTIL    = $(TAL)/util
TWEB     = $(TAL)/web
TPING    = $(TAL)/pingpong
LINK_SRC = $(PROTO)/Link.cpp $(PROTO)/LinkBaudSweep.cpp $(PROTO)/LinkFrameRx.cpp $(PROTO)/LinkArq.cpp $(PROTO)/LinkReorder.cpp $(PROTO)/LinkSweep.cpp $(PROTO)/ArqCache.cpp $(UTIL)/Log.cpp $(UTIL)/UtilCrc.cpp $(UTIL)/UtilCobs.cpp
AUTOLINK_SRC = $(SRC)/AutoLink.cpp $(LINK_SRC)

run_test_zzz: $(AUTOLINK_SRC) al/ZzzTest.cpp
\t$(CXX) $(CXXFLAGS) $(AUTOLINK_SRC) al/ZzzTest.cpp -o $@
'''
    manifest = run_generator(fake_mk, ['run_test_zzz'])
    # run_test_zzz links every file in $(AUTOLINK_SRC). Each
    # basename should show up in its src_for_* entry.
    expected = ['ArqCache', 'AutoLink', 'Link', 'LinkArq',
                'LinkBaudSweep', 'LinkFrameRx', 'LinkReorder',
                'LinkSweep', 'Log', 'UtilCobs', 'UtilCrc']
    for base in expected:
        m = re.search(r'^src_for_%s="([^"]*)"' % base,
                      manifest, re.MULTILINE)
        assert m, 'manifest missing src_for_%s' % base
        bins = m.group(1).split()
        assert 'run_test_zzz' in bins, (
            'src_for_%s does not include run_test_zzz — '
            'coverage would silently miss this new suite. '
            'This is the drift bug AGENTS.md rule 4 warns about.'
            % base)


def test_library_source_only_paths():
    # Negative pin: paths under al/ (the test tree) must NOT
    # appear in src_for_<basename>, even if they happen to share
    # a basename with a library source. The fix uses LIB_PREFIXES
    # to keep test code out of the merge map.
    fake_mk = '''\
SRC      = ../../src
AL       = $(SRC)/al
PROTO    = $(AL)/link
HAL      = $(AL)/hal
UTIL     = $(AL)/util
WEB      = $(AL)/web
PONG     = $(AL)/pingpong
TAL      = al
THAL     = $(TAL)/hal
TPROTO   = $(TAL)/link
TUTIL    = $(TAL)/util
TWEB     = $(TAL)/web
TPING    = $(TAL)/pingpong
LINK_SRC = $(PROTO)/Link.cpp $(PROTO)/LinkBaudSweep.cpp $(PROTO)/LinkFrameRx.cpp $(PROTO)/LinkArq.cpp $(PROTO)/LinkReorder.cpp $(PROTO)/LinkSweep.cpp $(PROTO)/ArqCache.cpp $(UTIL)/Log.cpp $(UTIL)/UtilCrc.cpp $(UTIL)/UtilCobs.cpp
AUTOLINK_SRC = $(SRC)/AutoLink.cpp $(LINK_SRC)

run_test_only_test: al/Link.cpp al/LinkTest.cpp
\t$(CXX) $(CXXFLAGS) al/Link.cpp al/LinkTest.cpp -o $@
'''
    manifest = run_generator(fake_mk, ['run_test_only_test'])
    # al/Link.cpp is a test-tree path. It must not produce a
    # src_for_Link entry (which would otherwise shadow the real
    # library Link.cpp coverage).
    assert 'src_for_Link' not in manifest, (
        'manifest incorrectly promoted test-tree al/Link.cpp '
        'to src_for_Link — test sources must be filtered out.')


def main():
    tests = [
        test_real_makefile_covers_every_test_bin,
        test_new_suite_picked_up_automatically,
        test_library_source_only_paths,
    ]
    for t in tests:
        sys.stdout.write('  %s ... ' % t.__name__)
        t()
        sys.stdout.write('PASS\n')
    print('=== coverage_manifest.py self-test PASS ===')


if __name__ == '__main__':
    main()
