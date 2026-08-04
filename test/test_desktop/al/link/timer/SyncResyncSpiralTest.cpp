// Regression wall for the SYNC resync death-spiral fixes.
//
// Field log (FireBeetle pair, 2026-07-06): 15 discs in 35 s, zero
// app throughput. Every re-lock at 512000 was followed ~600 ms later
// by `SYNC mid-message ACK timeout -> drop + BREAK`. 600 ms =
// Ping SETTLE_MS (100) + syncAckTimeoutMs (500): the FIRST chunk
// after each re-lock died into a peer still inside its own settle /
// baud-switch window, and onSyncAckTimeout dropped the link on the
// first waitForAck expiry with no retransmit — the recovery loop
// recreated its own trigger indefinitely.
//
// Four pins. Each fails when its fix is reverted.
//   1. SYNC retx ladder: an eaten ACK is re-sent up to maxRetx
//      before the link declares desync. ()
//   2. Post-lock TX admission gate: after a real drop, sendMsg is
//      held for postLockQuietMs, escalating with the disc streak.
//      A clean link (never dropped) is never gated. ()
//   3. reset_unlocked drains the HAL TX ring via discardTx so stale
//      pre-BREAK bytes don't eat the next admission. ()
//   4. SYNC NAK does not drive the ArqCache retx path (which would
//      resend a zero-byte frame). ()
#ifndef ARDUINO

#    include <cassert>
#    include <cstdint>
#    include <cstring>
#    include <iostream>
#    include <fstream>
#    include <sstream>
#    include <string>

#    include "al/AutoLinkConfig.h"
#    include "al/link/Link.h"
#    include "al/link/arq/ArqCache.h"
#    include "MockHal.h"
#    include "LinkTestAccessor.h"
#    include "TestCfg.h"

using namespace autolink;

namespace {

// Pin 1 — the ladder re-sends the same seq up to maxRetx before the
// caller gives up. Drive the production step directly; if the ladder
// is removed the step is a no-op and attempt never climbs.
void test_pin_sync_retx_ladder_resends_before_drop() {
    std::cout << "\n=== Pin 1: SYNC retx ladder resends before drop ==="
              << std::endl;
    ArqCache arq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.mode = AutoLinkConfig::Mode::SYNC;
    cfg.maxRetx = 3;
    cfg.syncAckTimeoutMs = 50;
    MockHal hal;
    Link l(hal, arq, true, cfg);
    LinkTestAccessor t(l);
    t.setSweepPhase(SweepPhase::NONE);
    // Force OK so sendMsgBegin admits.
    t.forceState(State::OK);

    uint8_t payload[8];
    memset(payload, 0xA5, sizeof payload);
    assert(t.syncBegin(payload, sizeof payload) && "begin must admit in OK");

    int before = hal.txBuf.size();
    // Simulate three consecutive RTO expiries (no ACK arriving).
    assert(t.syncRtoStep() && "attempt 1 must resend");
    assert(t.syncAttempt() == 1);
    assert(t.syncRtoStep() && "attempt 2 must resend");
    assert(t.syncRtoStep() && "attempt 3 must resend");
    assert(t.syncAttempt() == 3);
    // Ladder exhausted at maxRetx=3 -> the next step declines
    // (caller then drops + BREAKs).
    assert(!t.syncRtoStep() && "ladder must stop at maxRetx");
    assert(t.syncAttempt() == 3);
    assert((int)hal.txBuf.size() > before &&
           "each retx must put bytes on the wire");
    std::cout << "  PASS (3 resends, then honest stop at maxRetx)" << std::endl;
}

// Pin 2 — post-lock quiet gate. A link that just dropped holds
// sends for postLockQuietMs (x streak); a link that never dropped is
// never gated.
void test_pin_post_lock_quiet_gate() {
    std::cout << "\n=== Pin 2: post-lock TX admission gate ===" << std::endl;
    ArqCache arq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    cfg.mode = AutoLinkConfig::Mode::SYNC;
    cfg.postLockQuietMs = 600;
    MockHal hal;
    Link l(hal, arq, true, cfg);
    LinkTestAccessor t(l);

    // No disc yet -> never gated even right at lock.
    t.setLockedAt(hal.now);
    t.setRecentDiscs(0, 0);
    assert(!t.txQuiet() && "clean link must never be gated");

    // One disc, just re-locked -> gated for the quiet window.
    hal.now = 100000;
    t.setLockedAt(hal.now);
    t.setRecentDiscs(1, hal.now);
    assert(t.txQuiet() && "first frame after a drop must be held");

    // Past the window -> admitted.
    hal.now += cfg.postLockQuietMs + 1;
    assert(!t.txQuiet() && "gate must lift after postLockQuietMs");

    // Streak escalation: 3 discs => 3x window.
    hal.now += 10000;
    t.setLockedAt(hal.now);
    t.setRecentDiscs(3, hal.now);
    uint32_t lockedAt = hal.now;
    hal.now = lockedAt + cfg.postLockQuietMs + 1;
    assert(t.txQuiet() && "streak must widen the quiet window");
    hal.now = lockedAt + cfg.postLockQuietMs * 3 + 1;
    assert(!t.txQuiet() && "gate must lift after streak x window");
    std::cout << "  PASS (gated only after a real drop, escalates with streak)"
              << std::endl;
}

// Pin 3 — reset drains the HAL TX ring. The field log showed
// `backpressure n=57 arqPending=0` immediately after a re-lock:
// stale pre-BREAK bytes still queued. reset_unlocked must call
// discardTx.
void test_pin_reset_discards_tx_ring() {
    std::cout << "\n=== Pin 3: reset_unlocked drains the HAL TX ring ==="
              << std::endl;
    ArqCache arq{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    AutoLinkConfig cfg;
    testBaseCfg(cfg);
    MockHal hal;
    Link l(hal, arq, true, cfg);
    LinkTestAccessor t(l);
    int before = hal.discardTxCalls;
    t.resetLink(true);
    assert(hal.discardTxCalls == before + 1 &&
           "reset_unlocked must discardTx exactly once");
    std::cout << "  PASS (TX ring drained on reset)" << std::endl;
}

// Pin 4 — a SYNC NAK must not enter the ArqCache retx path. In SYNC
// the cache is never populated, so a cache-miss resend emits a
// zero-byte frame the peer treats as a seq advance. Source pin: the
// onNak retx call is guarded by mode != SYNC.
void test_pin_sync_nak_skips_cache_retx() {
    std::cout << "\n=== Pin 4: SYNC NAK skips ArqCache retx ===" << std::endl;
    std::string src;
    for (const char *path :
         { "../../src/al/link/io/LinkRx.cpp", "src/al/link/io/LinkRx.cpp",
           "../src/al/link/io/LinkRx.cpp" }) {
        std::ifstream f(path);
        if (f.good()) {
            std::stringstream ss;
            ss << f.rdbuf();
            src = ss.str();
            break;
        }
    }
    assert(!src.empty() && "LinkRx.cpp must be readable");
    // onNak returns early for SYNC before it can ever reach the
    // GBN window-resend call — the SYNC retx ladder (sendMsg's
    // blocking syncAwaitAcked) owns SYNC recovery instead.
    //
    // Anchored on the guard/resend ordering rather than on where
    // arq_.onNaked sits: onNaked moved below the SYNC return when
    // the NAK wake was added (it only reseats sentAtMs_, which
    // nothing on the SYNC path reads), and that move is not a
    // change to the contract this pin exists to protect.
    auto nak = src.find("bool Link::onNak(");
    assert(nak != std::string::npos);
    auto guard = src.find("cfg.mode == AutoLinkConfig::Mode::SYNC", nak);
    auto resend = src.find("gbnResendWindow_unlocked(", nak);
    assert(guard != std::string::npos && resend != std::string::npos &&
           guard < resend &&
           "the NAK-driven GBN resend must be guarded by mode != SYNC");
    // The SYNC branch must hand off to the ladder's wake, never to
    // a cache-backed resend: SYNC never populates the ArqCache, so
    // a resend there would ship a zero-byte frame the peer reads
    // as a seq advance.
    auto wake = src.find("noteNakWake(", nak);
    assert(wake != std::string::npos && wake < resend &&
           "the SYNC branch must wake the retx ladder");
    auto nakedPos = src.find("arq_.onNaked(missingCobsSeq", nak);
    assert(nakedPos != std::string::npos && nakedPos > guard &&
           "onNaked only reseats sentAtMs_, which the SYNC ladder "
           "never reads — it must sit on the ASYNC side of the guard");
    std::cout << "  PASS (SYNC NAK recovery owned by the ladder, not the "
                 "GBN window resend)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running SYNC Resync Spiral Fix Tests ===" << std::endl;
    test_pin_sync_retx_ladder_resends_before_drop();
    test_pin_post_lock_quiet_gate();
    test_pin_reset_discards_tx_ring();
    test_pin_sync_nak_skips_cache_retx();
    std::cout << "\n=== SYNC Resync Spiral Fix Tests Completed ==="
              << std::endl;
    return 0;
}

#endif
