// Pinned regression: reset_unlocked's fast preferredBaud_ relock
// (P3 direct re-lock, ~200-300ms) assumes the previously-locked
// baud is still trustworthy. Under repeated fast disconnects — the
// field log's seed event recurring, or the epoch-bounce before its
// own fix — that assumption is false: relocking straight back into
// whatever's causing the churn just repeats the failure. recentDiscs_
// already tracks fast-disconnect frequency (10s window); this pin
// covers reset_unlocked actually acting on it: once recentDiscs_
// reaches DISC_STORM_THRESHOLD, the fast-relock branch must be
// skipped even when preservePreferredBaud=true, isMaster, and a
// valid preferredBaud_ are all present — falling through to the
// full P1 walk instead.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <cstring>
#    include <cstdio>
#    include "MockHal.h"
#    include "LinkTestAccessor.h"
#    include "NullArqCache.h"
#    include "TestPaths.h"

using namespace autolink;

static const int kBauds[] = { 115200 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

static void bringToOk(Link &a, MockHal &mHal, MockHal &sHal, Link &b) {
    a.begin();
    b.begin();
    for (int i = 0; i < 200; i++) {
        mHal.pumpClock(50);
        sHal.pumpClock(50);
        pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        if (a.getState() == State::OK && b.getState() == State::OK)
            return;
    }
    assert(false && "failed to bring two single-baud nodes to OK");
}

// Pin 1: below threshold, the fast-relock branch still fires
// (PHASE3, resweepPrefPending_ armed) — the escalation must not be
// so aggressive it fires on the very first or second disconnect.
static void test_below_threshold_still_fast_relocks() {
    std::cout << "\n=== Pin 1: below DISC_STORM_THRESHOLD, fast relock "
                 "still fires (PHASE3) ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.syncAckTimeoutMs = 500;
    cfg.maxRetx = 50;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, mHal, sHal, b);
    LinkTestAccessor acc(a);

    // Preset recentDiscs_ to 1 (one prior fast disconnect); this
    // reset's own increment brings it to 2, still one below
    // DISC_STORM_THRESHOLD (3) — a normal, not-yet-a-storm level of
    // churn. lastDiscMs_ must be in the recent past relative to the
    // mock clock's CURRENT value, or reset_unlocked's own window
    // check (dnow - lastDiscMs_ < 10000) underflows and silently
    // resets recentDiscs_ to 1 regardless of the preset.
    acc.setRecentDiscs(1, mHal.now == 0 ? 1 : mHal.now);
    acc.resetLink(true, /*preserve=*/true, ResetReason::HealthWatchdog);
    assert(acc.sweepPhase() == SweepPhase::PHASE3 &&
           "Pin 1: below the storm threshold, reset_unlocked must still "
           "take the fast preferredBaud_ relock (PHASE3), not the full "
           "P1 walk — the escalation must not fire on ordinary churn");
    assert(acc.resweepPrefPendingForTest() &&
           "Pin 1: resweepPrefPending_ must be armed for the fast-relock "
           "path");
    std::cout << " Pin 1 PASS (recentDiscs_=2 < threshold, fast relock "
                 "still fires)"
              << std::endl;
}

// Pin 2: at threshold, the fast-relock branch is abandoned in
// favor of the full P1 walk.
static void test_at_threshold_forces_full_p1_walk() {
    std::cout << "\n=== Pin 2: at DISC_STORM_THRESHOLD, reset_unlocked "
                 "forces a full P1 walk instead of fast relock ==="
              << std::endl;
    NullArqCache cacheA, cacheB;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    cfg.syncAckTimeoutMs = 500;
    cfg.maxRetx = 50;
    cfg.mode = AutoLinkConfig::Mode::ASYNC;
    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;
    Link a(mHal, cacheA, /*isMaster=*/true, cfg);
    Link b(sHal, cacheB, /*isMaster=*/false, cfg);
    bringToOk(a, mHal, sHal, b);
    LinkTestAccessor acc(a);

    // Preset recentDiscs_ to 2 (two prior fast disconnects); this
    // reset's own increment brings it to exactly DISC_STORM_THRESHOLD
    // (3) — the one that must escalate.
    acc.setRecentDiscs(2, mHal.now == 0 ? 1 : mHal.now);
    acc.resetLink(true, /*preserve=*/true, ResetReason::HealthWatchdog);
    assert(acc.sweepPhase() == SweepPhase::PHASE1 &&
           "Pin 2: at the storm threshold, reset_unlocked must force the "
           "full P1 walk (SweepPhase::PHASE1), not the fast preferredBaud_ "
           "relock — relocking straight back into whatever's causing the "
           "churn just repeats the failure (the field log's repeating "
           "'P3 preferredBaud_ relock missed' cascade)");
    assert(!acc.resweepPrefPendingForTest() &&
           "Pin 2: resweepPrefPending_ must NOT be armed once the storm "
           "threshold escalates to a full walk — there's no fast-relock "
           "timeout to arm a fallback for");
    std::cout << " Pin 2 PASS (recentDiscs_=3 >= threshold, full P1 walk "
                 "forced)"
              << std::endl;
}

// Pin 3: source-grep on the escalation's load-bearing shape.
static void test_escalation_guard_source_grep() {
    std::cout << "\n=== Pin 3: preferredBaud_ fast-relock condition "
                 "includes recentDiscs_ < DISC_STORM_THRESHOLD "
                 "(source-grep) ==="
              << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/LinkCore.cpp").c_str(), "r");
    assert(f);
    char buf[65536]; // grown: LinkCore.cpp passed 16 KB and reset_unlocked was
                     // truncated mid-body
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    const char *fn = strstr(buf, "void Link::reset_unlocked");
    assert(fn);
    const char *body = strchr(fn, '{');
    assert(body);
    int depth = 0;
    const char *p = body;
    const char *end = nullptr;
    while (*p) {
        if (*p == '{')
            depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0) {
                end = p + 1;
                break;
            }
        }
        p++;
    }
    assert(end);
    int len = (int)(end - fn);
    char bodybuf[65536];
    if (len >= (int)sizeof(bodybuf))
        len = sizeof(bodybuf) - 1;
    memcpy(bodybuf, fn, len);
    bodybuf[len] = 0;

    // : the slave is now allowed the same
    // preserved-baud fast path on a HealthWatchdog
    // reset, so the fast-relock condition no longer
    // gates on `isMaster`. The structural pin is now
    // `preservePreferredBaud && wasEverOk_` (the
    // DISC_STORM_THRESHOLD escalation is unchanged).
    const char *cond = strstr(bodybuf, "preservePreferredBaud && wasEverOk_");
    assert(cond &&
           "Pin 3: the fast-relock condition must still check "
           "preservePreferredBaud && wasEverOk_ — the prior "
           "isMaster gate was removed so the slave can take the "
           "same preserved-baud path on a HealthWatchdog reset, "
           "but the post-OK gates (wasEverOk_, preferredBaud_, "
           "clampToMaxBauds, recentDiscs_ < DISC_STORM_THRESHOLD) "
           "are unchanged");
    // The DISC_STORM_THRESHOLD comparison must be part of the same
    // condition (within a short window after the anchor phrase,
    // before the branch's opening brace).
    const char *brace = strchr(cond, '{');
    assert(brace);
    int condLen = (int)(brace - cond);
    char condBuf[512];
    if (condLen >= (int)sizeof(condBuf))
        condLen = sizeof(condBuf) - 1;
    memcpy(condBuf, cond, condLen);
    condBuf[condLen] = 0;
    assert(strstr(condBuf, "recentDiscs_") != NULL &&
           strstr(condBuf, "DISC_STORM_THRESHOLD") != NULL &&
           "Pin 3: the preferredBaud_ fast-relock condition must include "
           "`recentDiscs_ < DISC_STORM_THRESHOLD` — without it, a fast-"
           "disconnect storm keeps relocking straight back into the same "
           "failure forever");
    std::cout << " Pin 3 PASS (recentDiscs_ < DISC_STORM_THRESHOLD is "
                 "part of the fast-relock condition)"
              << std::endl;
}

int main() {
    std::cout << "=== Disc-Storm Escalation Tests ===" << std::endl;
    test_below_threshold_still_fast_relocks();
    test_at_threshold_forces_full_p1_walk();
    test_escalation_guard_source_grep();
    std::cout << "\n=== All 3 disc-storm escalation pins PASS ===" << std::endl;
    return 0;
}

#endif
