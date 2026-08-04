// BREAK dispatch split out of LinkTimersOk.cpp to
// keep the per-file 15 KB cap. The onBreak /
// onBreakStorm methods are full Link members and
// live in this file rather than LinkBreak.cpp
// (which holds the free-function helpers and
// cannot access Link's private state).
#include "al/link/Link.h"
#include "al/link/timers/LinkBreak.h"
#include "al/util/Log.h"

namespace autolink {

namespace {
constexpr const char *TAG = "Link";
}

void Link::onBreak() {
    hw.lock();
    if (state != State::OK) {
        hw.clearAppBuf();
        hw.unlock();
        return;
    }
    // A UART_BREAK while OK is reported by the same driver path for
    // a genuine peer detach AND for a framing glitch — observed in
    // the field under sustained 512000-baud ASYNC traffic with
    // large messages. Debounce with a short confirm window so a
    // single glitch doesn't tear down a healthy link.
    //
    // The window is UNCONDITIONAL on link state, NOT gated on
    // pendingCount > 0: a burst that just drained to zero pending
    // is exactly the shape a real UART driver produces, and
    // gating on pending silently skips the debounce so a single
    // glitch fires the reset_unlocked path. A still-healthy link
    // keeps delivering valid frames through the window and
    // clears the suspicion; a genuinely dropped peer goes silent
    // and the deadline confirms the reset anyway.
    uint32_t now = hw.nowMs();
    // Coalesce: BREAK interrupts within LinkBreakConsts::BREAK_COALESCE_MS of
    // the prior one are treated as the same electrical event — the ESP32 UART
    // driver reports a single glitch as multiple BREAK / framing-error events
    // at sub-ms spacing, and the second-BREAK fast-confirm path must not be
    // reachable from a single glitch. Pinned by BreakInterruptCoalesceTest.
    if (breakSuspectMs_ != 0 &&
        (uint32_t)(now - breakSuspectMs_) <
            LinkBreakConsts::BREAK_COALESCE_MS) {
        // Re-arm the short confirm deadline; the prior arm is
        // still running and this is the same electrical event.
        hw.startTimer((int)breakConfirmMs_unlocked(*this));
        hw.unlock();
        return;
    }
    if (breakSuspectMs_ == 0) {
        // First BREAK: arm the confirm window. Must explicitly
        // re-arm a short timer here — whatever was previously
        // scheduled (e.g. the next idle-keepalive tick, seconds
        // out under a large idleTimeoutMs) would otherwise leave
        // the confirm check unevaluated for far longer than
        // LinkBreakConsts::BREAK_CONFIRM_MS, effectively never confirming or
        // clearing within any bounded window.
        breakSuspectMs_ = (now == 0) ? 1 : now;
        // Two-frame-clears: the first late-tail frame is the
        // "still in flight" data, the second proves the link
        // survived. Reset the seen counter to zero so the
        // first qualifying late-tail frame trips the
        // half-clear, not the full clear.
        breakSuspectSeen_ = 0;
        uint32_t confirmMs = breakConfirmMs_unlocked(*this);
        // The confirm deadline is a single one-shot RTOS timer
        // (xTimerCreate(..., pdFALSE, ...) on the real HAL) — it
        // fires exactly once, at expiry, with no intermediate
        // wake-up. An idle link's only route to clearing
        // suspicion is a CRC-valid PING/PONG round trip
        // (noteValidFrameOk_unlocked), and the only thing that
        // sends one is the keepalive check in onTimerOk_unlocked,
        // which never runs until the timer fires. If the
        // keepalive is due before the confirm deadline, arm the
        // FIRST wake-up there instead so onTimerOk_unlocked gets
        // a chance to send it before confirming a reset — without
        // this, a BREAK landing during an idle stretch always
        // confirms into a reset even against a fully healthy
        // peer. Pinned by BreakSuspectKeepaliveTest.
        uint32_t armMs = confirmMs;
        if (cfg.idleTimeoutMs > 0 && arq_.pendingCount() == 0) {
            uint32_t half = (uint32_t)cfg.idleTimeoutMs / 2;
            uint32_t sinceTx = (uint32_t)(now - lastTxMs);
            uint32_t dueIn = sinceTx >= half ? 0 : half - sinceTx;
            if (dueIn < armMs)
                armMs = dueIn;
        }
        if (armMs < 1)
            armMs = 1;
        hw.startTimer((int)armMs);
        // First BREAK does NOT clear the app buffer — the
        // suspect is unconfirmed, and a glitch on a healthy
        // link must not silently discard queued app data. The
        // clear moves to the confirm branch below; until then
        // the app layer still has its queued messages and a
        // genuine drop will tear down the link anyway.
        Log::log().info(TAG,
                        "BREAK suspect, confirm in %d ms (grace=%lu ms, "
                        "frames-needed=%d)",
                        (int)confirmMs,
                        (unsigned long)breakGraceMs_unlocked(*this),
                        (int)LinkBreakConsts::BREAK_GRACE_FRAMES_NEEDED);
        hw.unlock();
        return;
    }
    // A second BREAK outside the coalesce window while one is
    // already suspect is strong enough on its own — confirm at
    // once. The coalesce gate above guarantees two BREAKs from
    // one electrical glitch (sub-ms spaced) cannot reach this
    // fast-confirm path.
    //
    // But not if a qualifying frame has been observed since
    // the arm: that is the two-frame-clear contract. A
    // second BREAK after a valid late-tail frame proves the
    // link is alive, so re-arm the confirm deadline rather
    // than tearing down.
    if (breakSuspectSeen_ == 0) {
        breakSuspectMs_ = 0;
        Log::log().info(TAG, "BREAK -> resweep");
        // BREAK is a clean peer detach, so the proven baud is still
        // trustworthy and worth a fast P3 re-lock. Watchdog drops
        // clear it — that baud may have drifted.
        reset_unlocked(true, /*preservePreferredBaud=*/true,
                       ResetReason::HealthWatchdog);
        hw.unlock();
        return;
    }
    Log::log().info(TAG,
                    "BREAK during two-frame-clear window: re-arming "
                    "confirm deadline");
    breakSuspectSeen_ = 0;
    hw.startTimer((int)breakConfirmMs_unlocked(*this));
    hw.unlock();
}

void Link::onBreakStorm() {
    // Set a flag under the lock and return. The
    // UART event task that fired this hook holds the
    // driver lock and is also the RX drainer; calling
    // reset_unlocked inline would reenter setSpd and
    // deadlock the same shape the P3 branch in
    // LinkTimersSwp.cpp documents. The flag is
    // consumed at the top of onTimer() where the
    // state machine already owns the setSpd /
    // startTimer sequence. Pinned by
    // BreakStormDefersToOnTimerTest.
    hw.lock();
    breakStormPending_ = true;
    hw.unlock();
}

void Link::gbnResendWindow_unlocked(uint32_t now) {
    if (!arq_.gbnActive())
        return;
    uint8_t s = arq_.gbnBase();
    int pending = arq_.pendingCount();
    int burst = decideGbnResendCap(pending, cfg.gbnResendBurstMax);
    for (int i = 0; i < burst; i++) {
        retxSeq_unlocked(s);
        arq_.applyRetx(s, now);
        s = (s == COBS_SEQ_MAX) ? 0 : (uint8_t)(s + 1);
    }
}
} // namespace autolink