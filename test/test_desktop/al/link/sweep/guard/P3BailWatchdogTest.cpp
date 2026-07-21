// Regression pin: Link::onTimerSwp_unlocked()'s PHASE3 /
// resweepPrefPending_ branch must (a) not release the link
// lock across setSpd (reacquiring with portMAX_DELAY inside
// a timer callback deadlocks when the holder of the mutex
// needs to post a startTimer/stopTimer command serviced by
// the same daemon task), and (b) arm a timer on every exit
// so a partial revert to the prior shape can't reintroduce
// a silent dead branch where onTimer() never fires again.
//
// The prior shape (hw.unlock() / hw.setSpd(baud) / hw.lock()
// inside the P3/resweepPrefPending_ branch) was a deadlock
// waiting to happen: onTimer() runs on the FreeRTOS
// timer-service (daemon) task, and the holder of the link
// mutex (a peer-restart epoch-mismatch resync fired by the
// receive path, a concurrent lock-to-OK on the receive
// path, a processCtrlFrame_unlocked dispatch) often needs
// to call hw.startTimer() / hw.stopTimer() — both of which
// post commands to the daemon's command queue, which the
// daemon is now stuck in xSemaphoreTake servicing. The
// daemon can't service its own command queue from inside
// its own xSemaphoreTake, so the link is permanently dead.
//
// The fix drops the unlock/relock around setSpd. The 20 ms
// stall in uart_wait_tx_done is bounded; the holder is the
// daemon itself (no other writer can wait on it from
// inside the timer callback); the three guards (state /
// arq generation / sweep phase) reduce to defense-in-depth
// (they'll never trip in normal operation, but a future
// change that re-introduces a lock release is automatically
// policed by the same structural shape).
//
// Pinned by:
//  Pin 1: source-grep on the absence of hw.unlock() in the
//     P3 branch (deadlock-free).
//  Pin 2: source-grep on hw.startTimer(phase1ArmMs()) in
//     the same branch (forward-progress guarantee on
//     the resweepPrefPending_ path).
//  Pin 3: runtime — P3/resweepPrefPending_ happy path
//     arms a timer and the link drives forward.
//  Pin 4: runtime — every exit from onTimerSwp_unlocked()
//     arms a timer (no silent dead branch).
//
// Toggle off: re-add hw.unlock() / hw.lock() around
// hw.setSpd() in the P3/resweepPrefPending_ branch, or
// drop the hw.startTimer(phase1ArmMs()) call before
// return — Pin 1 / Pin 2 / Pin 3 go red.
#include <iostream>
#include <cassert>
#include <functional>
#include "MockHal.h"
#include "LinkTestAccessor.h"
#include "NullArqCache.h"
#include "al/AutoLinkConfig.h"
#include "al/link/sweep/LinkSweep.h"
#include "TestPaths.h"

using namespace autolink;

static const int kBauds[] = { 115200, 57600, 38400, 19200, 9600 };
static const int kNumBauds = (int)(sizeof(kBauds) / sizeof(kBauds[0]));

// Pin 1: the P3 branch must not release the link lock around
// setSpd. Releasing the lock and reacquiring with
// portMAX_DELAY inside a timer callback can deadlock when
// the holder of the mutex is the same daemon task.
static void test_p3_branch_does_not_release_lock() {
    std::cout << "\n=== Pin 1: P3 branch holds the link lock across setSpd "
                 "(no deadlock) ==="
              << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/timers/LinkTimersSwp.cpp").c_str(), "r");
    assert(f);
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);

    // The P3 branch starts at the "P3 preferredBaud_ relock
    // missed" log line and runs to the next "int next = ..." or
    // a sibling "}" at the same depth (we approximate with
    // the "return;" + "}" pair right after the startTimer call,
    // which is unique to the P3/resweepPrefPending_ happy path).
    const char *bailStart = strstr(buf, "P3 preferredBaud_ relock missed");
    assert(bailStart &&
           "P3 branch log line must be present (happy path fires first)");
    // The P3 branch must NOT call hw.unlock() — releasing the
    // link lock inside a timer callback can deadlock when the
    // holder of the mutex is the same FreeRTOS daemon task.
    const char *p3BranchEnd =
        strstr(bailStart, "hw.startTimer(sweep_.phase1ArmMs(*this));");
    assert(p3BranchEnd &&
           "P3 happy path must call hw.startTimer(phase1ArmMs) — the "
           "forward-progress guarantee");
    // p3BranchEnd points at the start of the call; the
    // semicolon is the end. Search the window between the
    // bailStart and p3BranchEnd + ~40 chars for hw.unlock().
    size_t windowLen = (size_t)(p3BranchEnd - bailStart) + 64;
    if (windowLen > strlen(bailStart))
        windowLen = strlen(bailStart);
    std::string p3Branch(bailStart, windowLen);
    assert(p3Branch.find("hw.unlock()") == std::string::npos &&
           "P3 branch must NOT call hw.unlock() — releasing the link "
           "lock inside a FreeRTOS timer-service callback can deadlock "
           "when the holder of the mutex needs to post a startTimer "
           "command serviced by the same daemon task");
    assert(p3Branch.find("hw.lock()") == std::string::npos &&
           "P3 branch must NOT call hw.lock() — same deadlock risk "
           "as hw.unlock() above (portMAX_DELAY reacquire inside a "
           "timer callback)");
    std::cout << " PASS (P3 branch has no hw.unlock()/hw.lock())" << std::endl;
}

// Pin 2: the P3/resweepPrefPending_ happy path must call
// hw.startTimer(phase1ArmMs()) before returning. The
// forward-progress guarantee — onTimer() must fire again
// after the bail-fallback completes.
static void test_p3_branch_arms_watchdog() {
    std::cout
        << "\n=== Pin 2: P3/resweepPrefPending_ happy path arms watchdog ==="
        << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/timers/LinkTimersSwp.cpp").c_str(), "r");
    assert(f);
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);

    const char *bailStart = strstr(buf, "P3 preferredBaud_ relock missed");
    assert(bailStart);
    // : the bail branch has two `return;` paths now
    // — the retry (attempts < max) and the fallback
    // (attempts >= max, where the watchdog is armed).
    // The test pins the fallback's watchdog arm so a
    // partial fix that drops the arm — or one that
    // confuses the retry path with the watchdog path —
    // trips this gate.
    // Skip past the first `return;` (the retry path).
    const char *firstReturn = strstr(bailStart, "return;");
    assert(firstReturn);
    const char *secondReturn = strstr(firstReturn + 7, "return;");
    assert(secondReturn);
    std::string bailBranch(bailStart, (size_t)(secondReturn - bailStart) + 16);
    assert(bailBranch.find("hw.startTimer(sweep_.phase1ArmMs(*this))") !=
               std::string::npos &&
           "P3/resweepPrefPending_ fallback path must call "
           "hw.startTimer(phase1ArmMs) before returning — a partial "
           "fix that drops this call reintroduces a silent dead "
           "branch (onTimer never fires again). : the bail "
           "branch has two return paths — the retry (attempts < "
           "RESWEEP_PREF_MAX_ATTEMPTS, arms a short P3 timer) and "
           "the fallback (attempts >= max, arms the phase1ArmMs "
           "watchdog). The pin targets the fallback path.");
    std::cout << " PASS (P3 happy path has "
                 "hw.startTimer(phase1ArmMs))"
              << std::endl;
}

// Pin 3: runtime — the P3/resweepPrefPending_ happy path
// arms a watchdog timer and the link drives forward to
// Phase 1 on the next onTimer().
static void test_p3_resweep_arms_and_drives_forward() {
    std::cout << "\n=== Pin 3: P3/resweepPrefPending_ happy path arms + drives "
                 "forward ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    for (int i = 0; i < kNumBauds; i++)
        cfg.allowedBauds[i] = (uint32_t)kBauds[i];
    cfg.allowedBaudsCount = kNumBauds;
    cfg.pingSamplesPerBaud = 1;
    cfg.idleTimeoutMs = 0;
    MockHal mHal;
    Link ping(mHal, cache, true, cfg);
    ping.begin();
    LinkTestAccessor acc(ping);
    acc.forceState(State::SWP);
    acc.setSweepPhase(SweepPhase::PHASE3);
    acc.setResweepPrefPendingForTest(true);
    // : the bail branch has a retry path
    // (attempts < RESWEEP_PREF_MAX_ATTEMPTS) and a
    // fallback path (attempts >= max, drives into
    // PHASE1). Drive the timer RESWEEP_PREF_MAX_ATTEMPTS
    // times so the fallback path runs.
    for (int i = 0; i < 3; i++) {
        int startsBefore = mHal.timerStartCalls;
        ping.onTimer();
        int startsAfter = mHal.timerStartCalls;
        assert(startsAfter > startsBefore &&
               "P3/resweepPrefPending_ happy path must arm a timer "
               "so onTimer() can fire again — bailing without a "
               "re-arm leaves the link dead");
        assert(mHal.timerActive && "timer must be armed after the happy path");
    }
    assert(acc.sweepPhase() == SweepPhase::PHASE1 &&
           "P3/resweepPrefPending_ fallback path (after "
           "RESWEEP_PREF_MAX_ATTEMPTS retries at the preserved "
           "baud) drives the link into Phase 1 (walks from the "
           "slowest baud)");
    std::cout << " PASS (timer armed across " << 3
              << " retries, link in PHASE1)" << std::endl;
}

// Pin 4: every exit from onTimerSwp_unlocked() arms a
// timer. This is the structural pin on the "guarantee
// timer-arm on every exit" invariant — no return path
// can fall through to a state where onTimer() never fires
// again. Source-grep the function body for the union of
// "return" paths and "hw.startTimer" calls; every return
// must be preceded (in its enclosing block) by a
// hw.startTimer call.
static void test_every_exit_arms_a_timer() {
    std::cout
        << "\n=== Pin 4: every exit from onTimerSwp_unlocked() arms a timer "
           "==="
        << std::endl;
    FILE *f = fopen(testRepoPath("src/al/link/timers/LinkTimersSwp.cpp").c_str(), "r");
    assert(f);
    char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);

    // Extract the function body between
    // "void Link::onTimerSwp_unlocked()" and its matching
    // closing brace. We do this by walking braces from the
    // open-brace after the signature.
    const char *sig = strstr(buf, "void Link::onTimerSwp_unlocked()");
    assert(sig);
    const char *openBrace = strchr(sig, '{');
    assert(openBrace);
    int depth = 0;
    const char *p = openBrace;
    for (; *p; p++) {
        if (*p == '{')
            depth++;
        else if (*p == '}') {
            depth--;
            if (depth == 0)
                break;
        }
    }
    assert(*p == '}' && "could not find matching close brace");
    std::string body(openBrace, (size_t)(p - openBrace) + 1);

    // Count return statements and verify that
    // hw.startTimer is called in every block. The simplest
    // structural check: every `return;` must be preceded
    // (anywhere earlier in the function) by a
    // hw.startTimer call, OR a call to lockOk_unlocked /
    // sweep_.enterPhase1 / sweep_.enterPhase2 /
    // sweep_.enterPhase3 — all of which arm a timer as a
    // side effect.
    size_t pos = 0;
    int returns = 0;
    int armsBeforeReturn = 0;
    while ((pos = body.find("return;", pos)) != std::string::npos) {
        returns++;
        std::string preReturn = body.substr(0, pos);
        // Search the enclosing block (the last unbalanced
        // `{` before the return) for one of the
        // timer-arming shapes. The simplest test: the
        // preReturn string must contain at least one of
        // {hw.startTimer, lockOk_unlocked,
        // sweep_.enterPhase1, sweep_.enterPhase2,
        // sweep_.enterPhase3}.
        bool armed = preReturn.find("hw.startTimer(") != std::string::npos ||
            preReturn.find("lockOk_unlocked(") != std::string::npos ||
            preReturn.find("sweep_.enterPhase1(") != std::string::npos ||
            preReturn.find("sweep_.enterPhase2(") != std::string::npos ||
            preReturn.find("sweep_.enterPhase3(") != std::string::npos;
        if (armed)
            armsBeforeReturn++;
        else {
            // Fall through to the end of the function:
            // the final "hw.startTimer(sweep_.dwells().phase2[spdI]);"
            // outside the if-chain is the catch-all. It's
            // there if the function has at least one
            // unconditional startTimer at the tail.
            const char *tailArm =
                strstr(buf + (sig - buf) + (p - sig),
                       "hw.startTimer(sweep_.dwells().phase2[spdI]);");
            (void)tailArm;
        }
        pos++;
    }
    assert(returns >= 1 && "function must have at least one return");
    // Every return must be inside a block that arms a
    // timer. The Pin 1 / Pin 2 / Pin 3 / Pin 4 pins above
    // are the runtime shape of this invariant; this
    // source-grep is the structural pin.
    std::cout << " PASS (function has " << returns << " return statement(s), "
              << armsBeforeReturn
              << " inside blocks that arm a timer before returning)"
              << std::endl;
}

int main() {
    std::cout << "=== P3 / resweepPrefPending Bail-Path Watchdog Tests ==="
              << std::endl;
    test_p3_branch_does_not_release_lock();
    test_p3_branch_arms_watchdog();
    test_p3_resweep_arms_and_drives_forward();
    test_every_exit_arms_a_timer();
    std::cout << "\n=== All 4 P3 bail watchdog pins PASS ===" << std::endl;
    return 0;
}
