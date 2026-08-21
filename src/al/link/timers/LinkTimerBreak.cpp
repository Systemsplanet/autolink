// BREAK dispatch split out of LinkTimersOk.cpp to
// keep the per-file 15 KB cap. The onBreak /
// onBreakStorm methods are full Link members and
// live in this file rather than LinkBreak.cpp
// (which holds the free-function helpers and
// cannot access Link's private state).
#include "al/link/Link.h"
#include "al/link/timers/LinkBreak.h"
#include "al/util/log/Log.h"

namespace autolink {

namespace {
constexpr const char *TAG = "Link";
}

void Link::onBreak() {
    // Bounded wait: this runs on the UART event task, which is
    // also the RX drainer. sendMsg can hold this same lock across
    // a blocking hw.tx() write; the old unconditional lock() would
    // park the event task behind it. A miss just drops this BREAK
    // notification — the debounce means the peer's next one (or
    // the confirm deadline, once armed) retries. Pinned by
    // EventTaskBoundedLockTest.
    if (!hw.tryLock(LinkBreakConsts::EVENT_TASK_LOCK_TIMEOUT_MS))
        return;
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
        // Confirming here means calling reset_unlocked, which
        // enters SWP via sweep_.enterPhase1/enterPhase3 —  both
        // call hw.setSpd() synchronously. Same reentrant-setSpd
        // hazard onBreakStorm() below defers for: this runs on the
        // UART event task, and the ESP-IDF UART driver already
        // holds its own internal lock while dispatching the event
        // that got us here. Defer the reset itself to the next
        // onTimer() tick (timer-daemon-task context, where
        // LinkTimersSwp.cpp's P3 branch establishes that holding
        // the link lock across setSpd is safe) and force that tick
        // to run almost immediately rather than wait for whatever
        // was previously scheduled. Pinned by
        // BreakOnBreakDefersToOnTimerTest.
        breakConfirmPending_ = true;
        hw.startTimer(1);
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
    //
    // Bounded wait, same reason as onBreak() above: a miss here
    // just waits for the next BREAK_SUMMARY_MS storm window to
    // retry. Pinned by EventTaskBoundedLockTest.
    if (!hw.tryLock(LinkBreakConsts::EVENT_TASK_LOCK_TIMEOUT_MS))
        return;
    breakStormPending_ = true;
    hw.unlock();
}

void Link::gbnResendWindow_unlocked(uint32_t now) {
    if (!arq_.gbnActive())
        return;
    uint8_t s = arq_.gbnBase();
    int pending = arq_.pendingCount();
    int burst = decideGbnResendCap(pending, cfg.gbnResendBurstMax);
    // Stamp the dedup pair BEFORE the burst so a NAK
    // arriving in the burst's flight window sees a
    // matching base+timestamp and is deduped. The dedup
    // logic lives in onNak (the same NAK that fired this
    // resend is the first one out of the gate; the second
    // NAK for the same loss event must not re-fire).
    // Pinned by GbnResendSameEventDedupeTest.
    gbnLastResendBase_ = s;
    gbnLastResendMs_ = now;
    // Frame size used by the txAvail() budget. The
    // shared kWorstCaseCobsFrame in LinkWire.h covers
    // the worst-case COBS encoding (preamble + rawLen
    // + 1:254 expansion + delim + seq + CRC8). The
    // The earlier bound (MAX_CHUNK + MSG_HDR + 2)
    // under-bounded the actual envelope by ~1 byte
    // and would let uart_write_bytes block on a
    // near-full ring. Pinned by
    // SendMsgTxAvailBoundTest.
    int perFrame = kWorstCaseCobsFrame;
    for (int i = 0; i < burst; i++) {
        // Bound the burst on the live TX-ring free space
        // so a full retx never blocks under the link
        // lock. Defect 1: the prior `for` ran to
        // completion and `hw.tx()` blocked until the
        // bytes were queued, holding the lock for ~31 ms
        // at 512000 baud with 8 frames queued. Abort the
        // burst and let the next OK-tick / onNak pick up
        // the remaining frames. Pinned by
        // UartTxBufferRetxBurstTest.
        if (hw.txAvail() < perFrame) {
            Log::log().debug(TAG,
                             "gbnResend: aborting burst at %d/%d — "
                             "tx ring low (free=%d, perFrame=%d)",
                             i, burst, hw.txAvail(), perFrame);
            break;
        }
        // F2: also break on a wire-stall return
        // (the pre-check passed but the write
        // itself failed — a true race). Skip
        // the applyRetx stamp for the failed
        // seq; the next OK-tick retries the
        // same seq on the next RTO budget.
        // Pinned by
        // ResendCobsFramePropagatesStallTest.
        if (!retxSeq_unlocked(s)) {
            Log::log().debug(TAG,
                             "gbnResend: aborting burst at %d/%d — "
                             "wire write failed for seq=%u",
                             i, burst, (unsigned)s);
            break;
        }
        // D13: pass the resend-source into applyRetx
        // so the RTO retx count advances only on the
        // timer-driven path, not on the NAK-driven
        // resend path. The storm-stuck verdict reads
        // retxCountFor(base) to decide whether the
        // link is genuinely stuck — a NAK storm that
        // never sees an RTO expiry must not satisfy
        // the "two real failed attempts" gate. NAK
        // bumps go into nakCount_[bi] (already split
        // from retxCount_); the RTO retx path
        // increments retxCount_[bi] and arms
        // sentAtMs_[bi] for the next RTO tick.
        // Pinned by GbnStuckNakCountGateTest
        // (a 17-NAK-in-65ms burst never advances
        // retxCountFor; the storm-stuck verdict
        // never opens).
        bool fromRto = (resendSource_ == ResendSource::Rto);
        arq_.applyRetx(s, now, fromRto);
        s = (s == COBS_SEQ_MAX) ? 0 : (uint8_t)(s + 1);
    }
}
} // namespace autolink