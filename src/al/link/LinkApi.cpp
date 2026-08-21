
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/util/log/Log.h"
#include "al/util/codec/UtilCrc.h"
#include <algorithm>
#include <cstring>

#ifdef ARDUINO
#    if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#        include <freertos/FreeRTOS.h>
#    endif
#endif

static constexpr const char *TAG = "AutoLink";

namespace autolink {
static_assert(
    MAX_CHUNK + MSG_HDR <= ArqCache::POOL_BUF_MAX,
    "MAX_CHUNK + MSG_HDR > ArqCache::POOL_BUF_MAX: bump POOL_BUF_MAX or shrink MAX_CHUNK");

void Link::err(FrameErrCause cause) {
    hw.lock();
    bool trigger = err_unlocked(cause);
    hw.unlock();
    if (trigger)
        hw.sendBreak();
}

bool Link::err_unlocked(FrameErrCause cause) {
    if (state != State::OK)
        return false;
    errs++;
    frameErrs++;
    // Per-cause stamp so the Stats surface
    // (badHeaderErrs / overLenErrs / crcFailErrs) is
    // always self-consistent: every err_unlocked bumps
    // exactly one cause field, the aggregate, and the
    // aggregate-vs-cause invariant (frameErrs == sum of
    // the three cause fields) is enforced by
    // FrameErrCauseTest. Pinned by FrameErrCauseTest.
    switch (cause) {
    case FrameErrCause::BadHeader:
        badHeaderErrs_++;
        break;
    case FrameErrCause::OverLen:
        overLenErrs_++;
        break;
    case FrameErrCause::CrcFail:
        crcFailErrs_++;
        break;
    }
    uint32_t now = hw.nowMs();
    if (now - errWindowStartMs_ >= 1000) {
        errWindowStartMs_ = now;
        errWindowCount_ = 1;
    } else {
        errWindowCount_++;
    }
    if (errs > cfg.errThreshold) {
        Log::log().info(TAG, "err threshold -> drop");
        reset_unlocked(true, false, ResetReason::ErrThreshold);
        return true;
    }
    if (cfg.errRateWindow > 0 && errWindowCount_ > cfg.errRateWindow) {
        Log::log().warning(TAG, "err rate -> resweep");
        reset_unlocked(true, false, ResetReason::ErrRate);
        return true;
    }
    return false;
}

void Link::clearErr() {
    hw.lock();
    if (errs > 0)
        errs = 0;
    hw.unlock();
}

int Link::available() const { return hw.appBufAvailable(); }
int Link::peek() { return hw.peekAppBuf(); }
int Link::read() {
    uint8_t b;
    return hw.popAppBuf(&b, 1) == 1 ? b : -1;
}
int Link::read(uint8_t *b, int max_len) { return hw.popAppBuf(b, max_len); }

int Link::readStream(uint8_t *b, int n) {
    int got = 0;
    while (got < n) {
        int r = hw.popAppBuf(b + got, n - got);
        if (r <= 0)
            break;
        got += r;
    }
    return got;
}

int Link::write(const uint8_t *b, int len) {
    if (len <= 0)
        return 0;
    hw.lock();
    int sent = sendMsg_unlocked(b, len);
    hw.unlock();
    return sent;
}

// Post-lock TX admission: hold sends only after a real link drop
// (the field failure is the first frame into a freshly re-locked
// peer that is still inside its settle / baud-switch window).
// Quiet window is baud-aware: cfg.postLockQuietMs is the
// per-disc baseline, but a 9600-baud re-lock needs a longer
// settle than 512000 (the round-trip time per frame differs
// by ~50x). Without the RTO floor the gate window is shorter
// than the time-to-drop at low baud and the app's first
// post-lock send is let through just before the health
// watchdog tears the link down again. Pinned by
// LinkHealthBaudAwareTest.
bool Link::txQuiet_unlocked() const {
    // Reverted a first-lock-only settle-cover hold: even scoped
    // to fire only once (firstLockAtMs_, never on a relock), any
    // fixed-duration hold here collides with
    // WireSimAppBufFullTest's ~1000-step budget, because
    // warmup_to_ok() returns within ~1-10ms of the lock event
    // (it polls every step and returns the instant both sides
    // read OK) — so the stress phase always starts deep inside
    // whatever hold window this uses, no matter how it's scoped.
    // A fixed-timer hold isn't a safe fix for the
    // post-lock-settle race: the peer's settle-drop
    // window closes on the first ACK/response, not on
    // a guessed duration.
    if (cfg.postLockQuietMs <= 0 || lockedAtMs_ == 0)
        return false;
    // AL90-1: gate applies to every lock,
    // including the first-ever lock. The field
    // capture's failure mode (84 chunks in
    // 741 ms with `reason=Kickoff`,
    // `recentDiscs_=0`) was the first lock of
    // a fresh session, not a re-lock — the
    // previous early-return left the first
    // lock with no admission hold. Pinned by
    // FirstLockAdmissionEvidenceGateTest.
    //
    // The first `sendMsg` of a lock cycle is
    // exempt from BOTH the wall-clock hold
    // and the event-driven early-clear: the
    // stamp in sendMsg() fires only on the
    // first send (when
    // `postLockFirstTxDone_ == lockedAtMs_`),
    // and the early-clear checks against that
    // stamp. Without this exemption, the
    // first send is deferred by the very gate
    // that exists to let it through, and the
    // unidirectional-send tests
    // (`test_sendmsg_stalls_when_arq_cache_full`,
    // `test_readme_usage`) hang. Pinned by
    // FirstLockAdmissionEvidenceGateTest.
    if (postLockFirstTxDone_ == lockedAtMs_)
        return false;
    // AL89-9: event-driven early-clear. The
    // wall-clock hold below is the time-based
    // gate (postLockQuietMs × m, floored at the
    // baud-aware RTO+chunk). Once the peer has
    // answered any CRC-valid post-lock packet
    // (firstPeerResponseSeen_), the wall-clock
    // hold is moot — the peer is live, the
    // settle window is closed, the rest of the
    // quiet window is dead time. Pinned by
    // FirstLockAdmissionEvidenceGateTest.
    if (postLockFirstTxDone_ != 0 && firstPeerResponseSeen_) {
        return false;
    }
    int m = recentDiscs_ > 4 ? 4 : recentDiscs_;
    int rto = baudAwareRtoMs_unlocked();
    // The quiet window is the larger of:
    //   - postLockQuietMs * m  (the baseline
    //     settle window, scaled by recent disc
    //     count), or
    //   - rto + one chunk transmit at the locked
    //     baud (the baud-aware floor, which is the
    //     minimum time the peer needs to actually
    //     answer the first frame).
    // At 9600 baud the rto + chunk budget is ~780
    // ms (520 ms RTO + 260 ms chunk), which is
    // larger than the 600 ms baseline at m=1, so
    // the floor activates there. At 115200 baud
    // the chunk transmit is ~22 ms, well under the
    // baseline, so the baseline wins — the floor
    // doesn't slow down a fast link, it only
    // protects a slow one. Pinned by
    // LinkHealthBaudAwareTest.
    uint32_t chunkTx = 0;
    uint32_t b = cfg.allowedBaudSafe(spdI);
    if (b > 0)
        chunkTx = (uint32_t)MAX_CHUNK * 10u * 1000u / b;
    uint32_t rtoFloor = (uint32_t)rto + chunkTx;
    uint32_t quiet = (uint32_t)cfg.postLockQuietMs * (uint32_t)m;
    if (rtoFloor > quiet)
        quiet = rtoFloor;
    return (uint32_t)(hw.nowMs() - lockedAtMs_) < quiet;
}

// SYNC retx ladder: on RTO expiry re-send the SAME seq verbatim
// (no txSeq advance). A garbled first copy arrives in-order on
// retry; a delivered copy whose ACK was lost re-ACKs as Stale.
// Only an exhausted ladder reaches the drop + BREAK policy below.
//
// F1: drain the TX ring before the retx write. The
// caller (syncAwaitAcked_unlocked via waitForAck)
// has already dropped the link lock, so the
// drain's unlock/reacquire here is a nested
// drop — but waitForAck re-acquires before
// returning, so the link lock IS held when
// this function is called. The drain drops
// it again (with a fresh deadline) — safe
// because the SYNC ladder's contract is
// "one seq, one RTO, one retry"; no other
// sendMsg is in flight on the link.
bool Link::syncRtoStep_unlocked(SyncOp &op) {
    // AL87-11: give-up is now time-based (elapsed since the
    // ladder's first step >= maxRetx * syncAckTimeoutMs), not
    // attempt-count-based. The two are IDENTICAL for the ladder's
    // designed case — a real RTO expiry every syncAckTimeoutMs —
    // so nothing changes for the normal silent-peer timeout path.
    // They diverge under a NAK storm: SyncNakFastRetxTest's wake
    // path lets waitForAck return almost immediately (a few ms)
    // on each NAK, so the OLD attempt-count cap let a storm of
    // NAKs 6ms apart burn all maxRetx=5 attempts in ~20-30ms and
    // declare mid-message desync + BREAK while the peer was
    // alive, in-sync, and actively telling the sender exactly
    // what it needed (an app-buf-full backpressure NAK, not wire
    // loss) — the field's 15-40s BREAK-storm-driven outages,
    // repeated every disc. The retx cadence itself stays
    // undamped and immediate (this codebase's NAK/resend
    // contract, deliberately not throttled — see LinkRx.cpp),
    // only the "how long do we tolerate this before giving up"
    // budget changed from ~any-N-attempts to the same wall-clock
    // patience the ladder always had for ordinary timeouts.
    // Pinned by SyncDropIsTimeBasedTest.
    uint32_t now = hw.nowMs();
    if (!op.started) {
        op.started = true;
        op.firstStepMs = now;
    }
    uint32_t budgetMs = (uint32_t)cfg.maxRetx * (uint32_t)cfg.syncAckTimeoutMs;
    if ((uint32_t)(now - op.firstStepMs) >= budgetMs)
        return false;
    op.attempt++;
    if (!drainTxRing_unlocked()) {
        Log::log().warning(TAG,
                           "SYNC retx seq=%u attempt=%d aborted — "
                           "ring didn't free within RTO",
                           (unsigned)op.seq, op.attempt);
        return false;
    }
    if (!buildAndTxCobsFrame_unlocked(op.seq, op.raw, op.rawLen)) {
        // F2: build failure means the frame
        // never went on the wire. Do NOT
        // advance arq_.onSent's notion of
        // "outstanding" (the seq is the
        // same, the chunk is the same; a
        // later syncRtoStep will try again
        // and the same seq is the legitimate
        // retry target).
        Log::log().warning(TAG,
                           "SYNC retx seq=%u attempt=%d aborted — "
                           "wire write failed",
                           (unsigned)op.seq, op.attempt);
        return false;
    }
    arq_.onSent(op.seq, NO_BASE, hw.nowMs(), txSeqLap_);
    lastTxMs = hw.nowMs();
    Log::log().warning(TAG, "SYNC retx seq=%u attempt=%d/%d", (unsigned)op.seq,
                       op.attempt, (int)cfg.maxRetx);
    return true;
}

bool Link::syncAwaitAcked_unlocked(SyncOp &op) {
    for (;;) {
        if (arq_.waitForAck(*this, op.seq, (uint32_t)cfg.syncAckTimeoutMs))
            return true;
        if (state != State::OK)
            return false;
        if (!syncRtoStep_unlocked(op))
            return false;
    }
}

// SYNC ACK-timeout policy. A mid-message timeout can leave the peer
// holding a partial length-prefixed message (framer desync): drop +
// BREAK now so both framers realign in one round-trip. A single
// merged-frame timeout can't wedge the peer; it only feeds the
// tx-reject streak. Returns true = caller sends BREAK after unlock.
bool Link::onSyncAckTimeout_unlocked(bool midMessage) {
    noteTxReject_unlocked();
    if (!midMessage || state != State::OK)
        return false;
    Log::log().warning(TAG,
                       "SYNC mid-message ACK timeout -> drop + BREAK (resync)");
    reset_unlocked(true, false, ResetReason::HealthWatchdog);
    return state == State::SWP;
}

int Link::sendMsg_unlocked(const uint8_t *b, int len) {
    if (state != State::OK) {
        Log::log().warning(TAG, "write: not OK -> dropped");
        return 0;
    }
    if (txQuiet_unlocked()) {
        Log::log().debug(TAG, "write: post-lock quiet -> deferred");
        return 0;
    }
    int offset = 0;
    while (offset < len) {
        if (state != State::OK)
            break;
        int chunk = std::min(len - offset, MAX_CHUNK);
        sendCobsFrame_unlocked(b + offset, chunk);
        txBytes += chunk;
        lastTxMs = hw.nowMs();
        offset += chunk;
        if (offset < len) {
            int gap = interChunkGapMs_unlocked();
            if (gap > 0)
                hw.delayUs((uint32_t)gap * 1000u);
        }
    }
    return offset;
}

void Link::dropLink() {
    hw.lock();

    if (state != State::OK) {
        hw.unlock();
        return;
    }
    reset_unlocked(true, false, ResetReason::UserDropLink);
    bool nb = (state == State::SWP);
    hw.unlock();
    if (nb)
        hw.sendBreak();
}

void Link::flush() { hw.flushTx(); }

void Link::flushRx() {
    hw.lock();
    hw.clearAppBuf();
    msgRx_.reset();
    msgRxStartedMs_ = 0;
    rxSeqSet = false;
    rxSeq = 0;
    hw.unlock();
    hw.flushRxHw();
}

bool Link::sendMsg(const uint8_t *b, int len, uint8_t *outBaseSeq,
                   SendResult *out) {
    if (len == 0) {
        if (outBaseSeq)
            *outBaseSeq = 0;
        lastSendMsgReason_ = SendMsgReason::LengthZero;
        return true;
    }
    if (len < 0 || (size_t)len > cfg.maxMsg) {
        if (outBaseSeq)
            *outBaseSeq = 0;
        lastSendMsgReason_ = SendMsgReason::LengthInvalid;
        return false;
    }
    const bool sync = (cfg.mode == AutoLinkConfig::Mode::SYNC);

    const int chunks = chunksForMsgLen(len);
    if (chunks <= 0 || chunks > COBS_SEQ_SPACE) {
        if (outBaseSeq)
            *outBaseSeq = 0;
        lastSendMsgReason_ = SendMsgReason::ChunksOverflow;
        return false;
    }
    hw.lock();
    if (state != State::OK) {
        hw.unlock();
        if (outBaseSeq)
            *outBaseSeq = 0;
        lastSendMsgReason_ = SendMsgReason::NotOk;
        return false;
    }
    if (txQuiet_unlocked()) {
        hw.unlock();
        postLockQuietDrops_++;
        Log::log().debug(TAG,
                         "sendMsg: post-lock quiet -> deferred (dropped=%lu)",
                         (unsigned long)postLockQuietDrops_);
        if (outBaseSeq)
            *outBaseSeq = 0;
        lastSendMsgReason_ = SendMsgReason::PostLockQuiet;
        return false;
    }

    // The in-flight set is always contiguous, so one bound covers
    // admission: inflight + this message's chunks must fit the
    // window. The 2x-sized pool always has room when that holds.
    const int inflight = sync ? 0 : arq_.pendingCount();
    const int window = arqCache_.window();
    if (!sync && inflight + chunks > window) {
        noteTxReject_unlocked();
        gbnWindowFullDrops_++;
        hw.unlock();
        Log::log().warning(TAG,
                           "sendMsg: GBN window full (inflight=%d + "
                           "chunks=%d > window=%d, dropped=%llu) — drop",
                           inflight, chunks, window,
                           (unsigned long long)gbnWindowFullDrops_);
        if (outBaseSeq)
            *outBaseSeq = 0;
        lastSendMsgReason_ = SendMsgReason::GbnWindowFull;
        return false;
    }
    // Offered-rate admission: refuse the message if
    // the projected line rate exceeds baud/10
    // bytes/s. The windowed byte counter is
    // refreshed on every successful send; the line
    // rate is 10 bits/byte (start + 8 data + stop).
    // A baud-mismatch downshift (512000 -> 9600)
    // without an offered-rate cap sends 8.3x the
    // line rate into a 32-chunk window — guaranteed
    // congestion collapse. Pinned by
    // RateLimitRolloverCheckTest.
    {
        uint32_t b = cfg.allowedBaudSafe(spdI);
        if (b > 0 && spdI >= 0 && spdI < cfg.allowedBaudsCount) {
            uint32_t now = hw.nowMs();
            uint32_t lineRateBps = b / 10u;
            // Debt gate: if a prior oversize admission
            // parked `rateNextAllowedMs_` in the future,
            // refuse until it elapses. Signed comparison
            // avoids the unsigned-wrap rollover that
            // would otherwise reset the window early.
            if ((int32_t)(now - rateNextAllowedMs_) < 0) {
                if (outBaseSeq)
                    *outBaseSeq = 0;
                ++rateLimitedCount_;
                lastSendMsgReason_ = SendMsgReason::RateLimited;
                hw.unlock();
                Log::log().warning(
                    TAG,
                    "sendMsg: rate-limited (deferred, "
                    "wait %ld ms, len=%d) — drop",
                    (long)((int32_t)rateNextAllowedMs_ - (int32_t)now), len);
                return false;
            }
            // Accrue drain credit: the wire has been
            // carrying bytes since the last admission.
            // `elapsed * lineRateBps / 1000` is the
            // byte budget the line has drained in
            // that time; deduct it from the window
            // counter, floored at zero. A burst that
            // the link is actively transmitting gets
            // credit proportional to wall time; a
            // host-test burst that hasn't advanced
            // the clock gets zero credit and the
            // payload check below still applies.
            // Pinned by sub-pin 5 (drain credit
            // recovers the byte counter) of
            // RateLimitRolloverCheckTest.
            if (rateWindowStartMs_ != 0 &&
                (int32_t)(now - rateWindowStartMs_) > 0) {
                uint32_t elapsedMs =
                    (uint32_t)((int32_t)now - (int32_t)rateWindowStartMs_);
                uint32_t drained =
                    (uint32_t)((uint64_t)elapsedMs * lineRateBps / 1000u);
                if (drained >= rateWindowBytes_) {
                    rateWindowBytes_ = 0;
                } else {
                    rateWindowBytes_ -= drained;
                }
            }
            // Debt cleared. Reset the window to
            // `now` and clear the byte counter so the
            // next message is metered against a fresh
            // budget. The previous shape parked
            // `rateWindowStartMs_` in the future too,
            // so the standard roll check
            // (`now - rateWindowStartMs_ >= RATE_WINDOW_MS`)
            // stayed false and `rateWindowBytes_`
            // accumulated a stale debt that re-parked
            // on every subsequent oversize call.
            // Pinned by RateLimitRolloverCheckTest.
            if (rateNextAllowedMs_ != 0) {
                // Just cleared a parked debt. Reset
                // both fields to the current time.
                rateWindowStartMs_ = now;
                rateWindowBytes_ = 0;
                rateNextAllowedMs_ = 0;
            } else if ((uint32_t)(now - rateWindowStartMs_) >= RATE_WINDOW_MS) {
                // Standard window rollover.
                rateWindowStartMs_ = now;
                rateWindowBytes_ = 0;
            }
            // Admission rule:
            // - Single-window messages: admit and
            //   charge the payload to the window
            //   counter. If `used + len > lineRateBps`
            //   after the drain credit has been
            //   applied, refuse with RateLimited —
            //   the app offered more than the
            //   line can carry in one second.
            // - Multi-window messages (oversize):
            //   admit and park a time-based debt
            //   equal to the transmission time.
            //   Subsequent offers of any size are
            //   refused via the debt gate until the
            //   parked time elapses. This is the
            //   documented pacing behavior:
            //   `send()` accepts `1..cfg.maxMsg`
            //   (the default 5120 B exceeds the
            //   960 B/s line rate at the slowest
            //   default baud) and the rate limiter
            //   paces admission rather than
            //   refusing oversize messages. Pinned
            //   by `RateLimitRolloverCheckTest`.
            if ((uint32_t)len > lineRateBps) {
                // Multi-window: admit, park debt.
                // The debt is purely time-based;
                // once it elapses, the next offer
                // (any size) is admitted against
                // a fresh window. The byte
                // counter is NOT updated for
                // multi-window — the next offer
                // will be paced by the debt gate
                // alone.
                uint32_t extraWindows =
                    ((uint32_t)len + lineRateBps - 1u) / lineRateBps;
                rateNextAllowedMs_ =
                    (int32_t)now + (int32_t)(extraWindows * RATE_WINDOW_MS);
            } else if (rateWindowBytes_ + (uint32_t)len > lineRateBps) {
                // Single-window over-budget: refuse.
                // The drain credit above has already
                // credited elapsed time, so the
                // window is the correct budget.
                if (outBaseSeq)
                    *outBaseSeq = 0;
                ++rateLimitedCount_;
                lastSendMsgReason_ = SendMsgReason::RateLimited;
                hw.unlock();
                Log::log().warning(TAG,
                                   "sendMsg: rate-limited (window full, "
                                   "len=%d + used=%lu > line=%lu B/s at "
                                   "baud=%lu) — drop",
                                   len, (unsigned long)rateWindowBytes_,
                                   (unsigned long)lineRateBps,
                                   (unsigned long)b);
                return false;
            } else {
                // Single-window admit. Charge the
                // payload (not wire bytes) so the
                // counter tracks what the app
                // offered, not what the wire
                // carried.
                rateWindowBytes_ += (uint32_t)len;
            }
        }
    }
    bool ok = true;
    bool brk = false;
    uint8_t baseSeq = 0;
    // AL90-1: stamp the first-TX marker on
    // EVERY lock (including first-ever) so the
    // event-driven early-clear in
    // txQuiet_unlocked has a baseline to
    // compare firstPeerResponseSeen_ against.
    // The first send of a fresh session still
    // needs to go out to produce the evidence;
    // the previous `recentDiscs_ > 0` qualifier
    // left the first lock's stamp unset and the
    // first send hung. Pinned by
    // FirstLockAdmissionEvidenceGateTest.
    if (postLockFirstTxDone_ == lockedAtMs_ && lockedAtMs_ != 0) {
        postLockFirstTxDone_ = hw.nowMs();
    }
    if (sync) {
        // Each frame gets a retx ladder (syncAwaitAcked): a single
        // lost/garbled frame or ACK is re-sent verbatim up to
        // cfg.maxRetx before the link declares mid-message desync.
        if (len + MSG_HDR <= MAX_CHUNK) {
            uint8_t merged[MAX_CHUNK];
            msgHdrEncode((uint32_t)len, UtilCrc::crc16(b, len), merged);
            memcpy(merged + MSG_HDR, b, len);
            SyncOp op;
            op.seq = txSeq;
            op.raw = merged;
            op.rawLen = MSG_HDR + len;
            // F1: SYNC branch drains the TX
            // ring before each write. The
            // drain drops the link lock — safe
            // because sendMsg is called from
            // the app task, not onRx, and the
            // SYNC ladder's contract is "one
            // seq, one RTO". A drain failure
            // aborts the message with
            // SyncMidMessageTimeout (existing
            // failure mode).
            if (!drainTxRing_unlocked()) {
                ok = false;
                lastSendMsgReason_ = SendMsgReason::SyncMidMessageTimeout;
                Log::log().warning(TAG,
                                   "sendMsg (SYNC, single): ring didn't free — "
                                   "abort before header");
            } else if (!sendCobsFrame_unlocked(merged, MSG_HDR + len)) {
                // F2: wire write failed. The
                // frame never went out. Abort
                // with SyncMidMessageTimeout.
                ok = false;
                lastSendMsgReason_ = SendMsgReason::SyncMidMessageTimeout;
                Log::log().warning(
                    TAG,
                    "sendMsg (SYNC, single): wire write refused — "
                    "abort before seq advance");
            } else {
                txBytes += len;
                lastTxMs = hw.nowMs();
                arq_.onSent(op.seq, NO_BASE, hw.nowMs(), txSeqLap_);
                if (!syncAwaitAcked_unlocked(op)) {
                    ok = false;
                    brk = onSyncAckTimeout_unlocked(false);
                }
                baseSeq = op.seq;
            }
        } else {
            uint8_t hdrOnly[MSG_HDR];
            msgHdrEncode((uint32_t)len, UtilCrc::crc16(b, len), hdrOnly);
            SyncOp hop;
            hop.seq = txSeq;
            hop.raw = hdrOnly;
            hop.rawLen = MSG_HDR;
            // F1 (AL89-1): pre-header reservation removed.
            // SYNC's multi-chunk path ACKs every chunk via
            // syncAwaitAcked_unlocked, so the chunks are
            // never in the ring simultaneously. The
            // `chunks + 1` reservation was a math maximum
            // that, against a ring sized exactly to
            // kWorstCaseCobsFrame * (msgChunks + 1),
            // demanded the entire ring before any byte
            // went out — txAvail could never report the
            // full ring, drainTxRing_unlocked spun the
            // whole 500 ms RTO, and every message > 2000 B
            // was dropped. Pinned by
            // SyncMultiChunkDrainRemovedTest.
            if (!drainTxRing_unlocked()) {
                ok = false;
                lastSendMsgReason_ = SendMsgReason::SyncMidMessageTimeout;
                Log::log().warning(TAG,
                                   "sendMsg (SYNC, multi): ring didn't free — "
                                   "abort before header");
            } else if (!sendCobsFrame_unlocked(hdrOnly, MSG_HDR)) {
                ok = false;
                lastSendMsgReason_ = SendMsgReason::SyncMidMessageTimeout;
                Log::log().warning(
                    TAG,
                    "sendMsg (SYNC, multi): wire write refused — "
                    "abort before seq advance");
            } else {
                baseSeq = hop.seq;
                lastTxMs = hw.nowMs();
                arq_.onSent(hop.seq, NO_BASE, hw.nowMs(), txSeqLap_);
                if (!syncAwaitAcked_unlocked(hop)) {
                    ok = false;
                    brk = onSyncAckTimeout_unlocked(true);
                }
                int offset = 0;
                while (offset < len && ok) {
                    if (state != State::OK) {
                        ok = false;
                        break;
                    }
                    int chunk = std::min(len - offset, MAX_CHUNK);
                    SyncOp cop;
                    cop.seq = txSeq;
                    cop.raw = b + offset;
                    cop.rawLen = chunk;
                    if (!drainTxRing_unlocked() ||
                        !sendCobsFrame_unlocked(b + offset, chunk)) {
                        // AL91-1 (was AL90-12): bound the
                        // damage. A mid-loop drain failure
                        // calls the abort path WITHOUT
                        // teardown — the link stays OK
                        // and the peer's per-frame RTO
                        // (syncAckTimeoutMs = 500) times
                        // out waiting for the next frame
                        // and resyncs on its own. The
                        // SyncMidMessageTimeout reason
                        // stamps the failure for the
                        // field operator. Pinned by
                        // SyncMidLoopDrainAbortTest.
                        ok = false;
                        lastSendMsgReason_ =
                            SendMsgReason::SyncMidMessageTimeout;
                        Log::log().warning(TAG,
                                           "sendMsg (SYNC, multi loop): "
                                           "ring/wire failed at offset %d/%d — "
                                           "partial send, link stays OK",
                                           offset, len);
                        // No BREAK: peer's RTO will
                        // resync. We abandon THIS
                        // message only.
                        break;
                    }
                    txBytes += chunk;
                    lastTxMs = hw.nowMs();
                    arq_.onSent(cop.seq, NO_BASE, hw.nowMs(), txSeqLap_);
                    if (!syncAwaitAcked_unlocked(cop)) {
                        // AL91-1: an ACK timeout in the
                        // multi-loop is also bounded —
                        // the per-frame RTO exhausted
                        // the retx ladder (cfg.maxRetx
                        // rounds). A full link reset
                        // here is the legacy shape; the
                        // peer resyncs on its own next
                        // RTO. Stamp the failure
                        // reason and abandon THIS
                        // message.
                        ok = false;
                        lastSendMsgReason_ =
                            SendMsgReason::SyncMidMessageTimeout;
                        Log::log().warning(TAG,
                                           "sendMsg (SYNC, multi loop): "
                                           "ACK ladder exhausted at "
                                           "offset %d/%d — partial send, "
                                           "link stays OK",
                                           offset, len);
                        break;
                    }
                    offset += chunk;
                }
            }
        }
    } else {
        if (len + MSG_HDR <= MAX_CHUNK) {
            uint16_t c = UtilCrc::crc16(b, len);
            uint8_t hdr[MSG_HDR] = { (uint8_t)(len),       (uint8_t)(len >> 8),
                                     (uint8_t)(len >> 16), (uint8_t)(len >> 24),
                                     (uint8_t)(c),         (uint8_t)(c >> 8) };
            uint8_t merged[MAX_CHUNK];
            memcpy(merged, hdr, MSG_HDR);
            memcpy(merged + MSG_HDR, b, len);
            // D3: 0xFF refusal propagates. Without the
            // check, the caller stamps 0xFF as a valid
            // baseSeq, the message is gone, and the
            // operator sees "sendMsg returned true" with
            // zero wire bytes. Pinned by
            // SendCobsFrameAckedRefusalPropagatesTest
            // (pool-exhausted single-frame sendMsg
            // returns false, *outBaseSeq == 0, no
            // bytes on the wire).
            baseSeq =
                sendCobsFrameAcked_unlocked(merged, MSG_HDR + len, NO_BASE);
            if (baseSeq == 0xFF) {
                noteTxReject_unlocked();
                poolExhaustDrops_++;
                lastSendMsgReason_ = SendMsgReason::PoolExhaust;
                Log::log().warning(
                    TAG,
                    "sendMsg: single-frame send refused (pool exhausted) "
                    "— no wire bytes emitted");
                hw.unlock();
                if (brk)
                    hw.sendBreak();
                if (outBaseSeq)
                    *outBaseSeq = 0;
                return false;
            }
            txBytes += len;
            lastTxMs = hw.nowMs();
        } else {
            uint16_t c = UtilCrc::crc16(b, len);
            uint8_t hdrOnly[MSG_HDR] = {
                (uint8_t)(len),       (uint8_t)(len >> 8), (uint8_t)(len >> 16),
                (uint8_t)(len >> 24), (uint8_t)(c),        (uint8_t)(c >> 8)
            };
            // Partial-send abort path (defect 5 in the
            // field-log analysis): the earlier code sent the
            // header first, then checked arqCache_.hasRoom()
            // per chunk in the loop below. A mid-message
            // pool-exhaust broke the loop with the header
            // already on the wire; the peer's msgRx_ then
            // consumed the *next* message's body bytes as
            // this one's continuation — a stream desync
            // F2 (F3 part 1): pre-header
            // wire-stall check. If the ring
            // can't fit the hdr frame, abort
            // the message with TxRingStall
            // before any data chunk goes out.
            // The old code path let the header
            // write attempt happen, got 0xFF
            // back, and conflated it with
            // PoolExhaust (a single counter
            // absorbed both failures — see
            // F3 for the symmetric fix on the
            // post-D3 rejection path). Pinned
            // by TxRingStallReasonTest.
            if (!sync && hw.txAvail() < kWorstCaseCobsFrame) {
                txRingStallDrops_++;
                lastSendMsgReason_ = SendMsgReason::TxRingStall;
                Log::log().warning(
                    TAG,
                    "sendMsg: TX ring can't fit header (free=%d, "
                    "perFrame=%d) — drop before header",
                    hw.txAvail(), kWorstCaseCobsFrame);
                hw.unlock();
                if (brk)
                    hw.sendBreak();
                if (outBaseSeq)
                    *outBaseSeq = 0;
                return false;
            }
            // recoverable only via a CRC fail plus resync
            // scan. Hoist the room check to cover the full
            // chunk set up front, and reject the entire
            // message before the header goes out.
            //
            // The check is `freeRoom() < chunks` (not
            // `!hasRoom() && freeRoom() < chunks`) — the
            // nested-`!hasRoom()` gate is unreachable when
            // exactly one slot is free but `chunks > 1`:
            // `hasRoom()` returns true, the inner check
            // never runs, the header goes out, and the
            // loop breaks at chunk 2 with a partial
            // message on the wire. Pinned by
            // SendMsgRoomCheckBeforeHeaderTest
            // (freeRoom=1, chunks=22 -> guard fires).
            if (!sync && arqCache_.freeRoom() < chunks) {
                noteTxReject_unlocked();
                poolExhaustDrops_++;
                lastSendMsgReason_ = SendMsgReason::PoolExhaust;
                Log::log().warning(
                    TAG,
                    "sendMsg: ARQ cache lacks room for full message "
                    "(need=%d chunks, free=%d) — drop before header",
                    chunks, arqCache_.freeRoom());
                hw.unlock();
                if (brk)
                    hw.sendBreak();
                if (outBaseSeq)
                    *outBaseSeq = 0;
                return false;
            }
            baseSeq = sendCobsFrameAcked_unlocked(hdrOnly, MSG_HDR, NO_BASE);
            // D3: the 0xFF return from sendCobsFrameAcked_unlocked
            // is a refusal, not a success. A successful header
            // returns a non-0xFF seq; a refused header returns
            // 0xFF and the entire message is dropped with
            // PoolExhaust. Without this check the call site
            // would treat 0xFF as a valid seq and the next
            // chunk's onSent would overwrite every prior
            // message's baseSeq_ with seq — destroying
            // message grouping.
            if (baseSeq == 0xFF) {
                // D3 + F2: the 0xFF refusal
                // from sendCobsFrameAcked_unlocked
                // here is unambiguously
                // PoolExhaust — the F2
                // pre-header wire-stall check
                // above has already caught a
                // ring-too-full condition with
                // the TxRingStall reason. The
                // race between the pre-header
                // check and the write
                // (arqCache_.hasRoom() flips
                // false) is a pool-exhaust
                // event, not a wire event.
                noteTxReject_unlocked();
                ok = false;
                lastSendMsgReason_ = SendMsgReason::PoolExhaust;
                Log::log().warning(
                    TAG,
                    "sendMsg: header send refused — pool exhausted, "
                    "aborting before any data chunk");
                hw.unlock();
                if (brk)
                    hw.sendBreak();
                if (outBaseSeq)
                    *outBaseSeq = 0;
                return false;
            }
            int offset = 0;
            // D4 / D5: the inter-chunk pacing gap and
            // the txAvail drain wait must release the
            // lock and use vTaskDelay (delayMs) so the
            // loop task yields to onRx / onTimer. Pinned
            // by SendMsgDrainYieldsAndBoundsTest (a HAL
            // that counts the inter-lock interval sees
            // >= 1 ms per chunk; the lock-acquire count
            // matches the chunk count + 1).
            //
            // E1: the per-message deadline that was
            // computed once before the loop is wrong
            // for a 22-chunk maxMsg at 512000 baud —
            // a single baudAwareRtoMs_unlocked is
            // sized for ONE chunk's wire time, not
            // the whole message. The unwalked shape
            // aborted a healthy 487 ms send at 115200
            // against a 500 ms deadline, exactly the
            // partial-message stream desync D2 was
            // supposed to eliminate. Re-arm at the
            // top of each drain wait (per-wait, not
            // per-message) so the deadline is the
            // freshly-spent chunk's RTO + the time
            // the ring is taking to drain.
            int perFrame = kWorstCaseCobsFrame;
            while (offset < len && ok) {
                if (state != State::OK) {
                    ok = false;
                    break;
                }

                if (!arqCache_.hasRoom()) {
                    noteTxReject_unlocked();
                    poolExhaustDrops_++;
                    Log::log().warning(
                        TAG,
                        "sendMsg: ARQ cache exhausted mid-message "
                        "(emitted %d/%d bytes, dropped=%llu) — partial send",
                        offset, len, (unsigned long long)poolExhaustDrops_);
                    lastSendMsgReason_ = SendMsgReason::PoolExhaust;
                    ok = false;
                    break;
                }
                // D6: bound the chunk send on the live
                // TX-ring free space. The bound is the
                // worst-case COBS frame (kWorstCaseCobsFrame
                // in LinkWire.h) so the ring never blocks
                // under the lock; a stuck ring trips the
                // deadline (D4) and is reported as
                // SendMsgReason::TxRingStall so the app can
                // retry on its own schedule rather than
                // hang the call forever. The bound applies
                // to both ASYNC and SYNC (the SYNC path's
                // sendCobsFrame_unlocked goes through hw.tx,
                // which blocks the same way on a full ring;
                // see the per-frame drain in LinkTx.cpp's
                // buildAndTxCobsFrame_unlocked for the
                // SYNC-side mirror).
                if (hw.txAvail() < perFrame) {
                    // G4: the ASYNC multi-chunk loop now calls
                    // drainTxRing_unlocked (the single drain implementation)
                    // instead of carrying its own inline copy. The inline
                    // copy existed because the ASYNC path needed the
                    // arqCache_.hasRoom() check after re-acquire; that check
                    // now lives in drainTxRing_unlocked itself, called with
                    // an outReason that stamps the right diagnostic
                    // (PoolExhaust, TxRingStall, ResetDuringSend). Pinned by
                    // AsyncLoopCallsDrainTxRingTest.
                    SendMsgReason r = SendMsgReason::None;
                    if (!drainTxRing_unlocked(&r)) {
                        if (r != SendMsgReason::None)
                            lastSendMsgReason_ = r;
                        ok = false;
                        break;
                    }
                    continue;
                }
                int chunk = std::min(len - offset, MAX_CHUNK);
                // D3 + D7: sendCobsFrameAcked_unlocked's 0xFF
                // return propagates — the chunk was not
                // stamped, not pending, not in the cache.
                // The whole message is partial and the
                // sender has lied about its len. Fail with
                // PoolExhaust, no offset advance, no
                // txBytes accounting for the refused chunk.
                uint8_t chunkSeq =
                    sendCobsFrameAcked_unlocked(b + offset, chunk, baseSeq);
                if (chunkSeq == 0xFF) {
                    noteTxReject_unlocked();
                    poolExhaustDrops_++;
                    lastSendMsgReason_ = SendMsgReason::PoolExhaust;
                    Log::log().warning(
                        TAG,
                        "sendMsg: chunk %d/%d refused (pool exhausted), "
                        "aborting — message is partial",
                        offset, len);
                    ok = false;
                    break;
                }
                txBytes += chunk;
                lastTxMs = hw.nowMs();
                offset += chunk;
                // D5: the inter-chunk gap is held outside
                // the link lock. ets_delay_us is a CPU
                // spin and the previous 1 ms busy-wait
                // under hw.lock() starved the peer end
                // of the link for ~107 ms per max-size
                // message. Pinned by
                // SendMsgDrainYieldsAndBoundsTest.
                if (offset < len) {
                    int gap = interChunkGapMs_unlocked();
                    if (gap > 0) {
                        hw.unlock();
                        hw.delayUs((uint32_t)gap * 1000u);
                        hw.lock();
                        if (state != State::OK) {
                            ok = false;
                            break;
                        }
                    }
                }
            }
        }
    }
    if (ok)
        txRejFirstMs_ = txRejLastMs_ = 0;
    // Stamp the success / specific-failure reason before
    // unlock so the app can read lastSendMsgReasonForTest()
    // after sendMsg returns without racing the link thread.
    // A specific failure reason (GbnWindowFull / PoolExhaust /
    // SyncMidMessageTimeout) is preserved — the Ok/NotOk
    // stamp here is conditional so it doesn't overwrite a
    // more precise cause with a conflated `NotOk`. Pinned
    // by SendMsgReasonEnumTest.
    if (ok || lastSendMsgReason_ == SendMsgReason::Ok) {
        lastSendMsgReason_ = ok ? SendMsgReason::Ok : SendMsgReason::NotOk;
    }
    hw.unlock();

    // Wire op after the single unlock (onTimer discipline).
    if (brk)
        hw.sendBreak();
    if (outBaseSeq)
        *outBaseSeq = baseSeq;
    // E8: surface the txSeqLap_ alongside
    // baseSeq so the app can pair the
    // bytesRecvdForMessage(baseSeq, baseLap)
    // query. The lap is the cobsSeq wrap
    // count at the time the header was
    // stamped — the same value the production
    // code stamps into baseLap_[bi] in
    // arq_.onSent. Pinned by
    // SendMsgReturnsBaseLapTest.
    if (out) {
        out->baseSeq = baseSeq;
        out->baseLap = txSeqLap_;
    }
#ifdef ARDUINO
    portYIELD();
#endif
    return ok;
}

int Link::recvMsg(uint8_t *out, int max_len) {
    hw.lock();
    // AL92-3: a partial message that has sat
    // "in progress" too long is abandoned and
    // resynced rather than left to splice onto
    // whatever arrives next. Checked before the
    // normal !inMsg()/beginMsg dispatch so a
    // stale partial never gets the chance to
    // consume a legitimate new message's bytes.
    if (msgRx_.inMsg() && msgRxStartedMs_ != 0) {
        uint32_t staleMs = (uint32_t)(hw.nowMs() - msgRxStartedMs_);
        // 2 * syncAckTimeoutMs is a SYNC-shaped constant; ASYNC's
        // own recovery ladder (LinkTimersGbn.cpp) can legitimately
        // run a single retx round out to
        // maxRetx * baudAwareRtoMs + gbnBackoffCapMs_unlocked() —
        // well past 2 * syncAckTimeoutMs at the defaults — so a
        // message still being repaired must not be abandoned
        // mid-repair. Take the larger of the two horizons; SYNC
        // (whose ladder is bounded by syncRtoStep_unlocked's own
        // budget, not this path) keeps its smaller horizon. Pinned
        // by StaleMessageAbandonedTest.
        uint32_t syncFloorMs = 2u * (uint32_t)cfg.syncAckTimeoutMs;
        uint32_t asyncFloorMs = (uint32_t)cfg.maxRetx *
                (uint32_t)baudAwareRtoMs_unlocked() +
            gbnBackoffCapMs_unlocked();
        uint32_t staleLimitMs =
            asyncFloorMs > syncFloorMs ? asyncFloorMs : syncFloorMs;
        if (staleMs > staleLimitMs) {
            msgRx_.reset();
            msgRxStartedMs_ = 0;
            int drop = findMsgHeaderResync_unlocked(cfg.maxMsg + MSG_HDR);
            Log::log().warning(
                TAG,
                "recvMsg: partial message abandoned after %lu ms "
                "(limit %lu ms) — resync scan dropped %d bytes",
                (unsigned long)staleMs, (unsigned long)staleLimitMs, drop);
            hw.unlock();
            err(FrameErrCause::BadHeader);
            return -1;
        }
    }
    if (!msgRx_.inMsg()) {
        if (hw.appBufAvailable() < MSG_HDR) {
            hw.unlock();
            return 0;
        }
        uint8_t h[MSG_HDR];
        readStream(h, MSG_HDR);
        if (!msgRx_.beginMsg(h, cfg.maxMsg)) {
            int drop = findMsgHeaderResync_unlocked(cfg.maxMsg + MSG_HDR);
            // Any resync scan that returns 0 or -1
            // after beginMsg failed is a genuine
            // frame error and must count the same
            // way any other frame-error path does;
            // otherwise the errThreshold /
            // errRateWindow paths never see the
            // bad-header case and a real CRC / desync
            // stream keeps the link up. Pinned by
            // ResyncScanErrTest.
            (void)drop;
            // BadHeader cause: tag this fire so the
            // badHeaderErrs counter on Stats can
            // disambiguate a 6-byte bad-header stream from
            // a CRC-failure stream (CrcFail) or an
            // oversize-length stream (OverLen) — three
            // different field problems, all
            // err() / err_unlocked callers today. Pinned
            // by FrameErrCauseTest. Rate-limited (≥1000 ms)
            // diagnostic so a real resync storm doesn't
            // flood the log at the ASYNC pipeline rate.
            // The first fire inside a window emits a
            // single sample line carrying both the cause
            // and the findMsgHeaderResync_unlocked return
            // value, then the gate is latched for the
            // rest of the window. The aggregate
            // badHeaderErrs counter increments every fire
            // — the gate is on the LOG, not the counter.
            {
                uint32_t nowMs = hw.nowMs();
                if (lastBadHeaderLogMs_ == 0 ||
                    (uint32_t)(nowMs - lastBadHeaderLogMs_) >= 1000) {
                    lastBadHeaderLogMs_ = nowMs;
                    Log::log().debug(TAG,
                                     "frame err: cause=BadHeader resyncDrop=%d "
                                     "(badHeaderErrs=%llu total)",
                                     drop, (unsigned long long)badHeaderErrs_);
                }
            }
            hw.unlock();
            err(FrameErrCause::BadHeader);
            return -1;
        }
        // AL92-3: stamp the start of this
        // partial message so the staleness
        // check above can bound it.
        msgRxStartedMs_ = hw.nowMs();
    }
    if (hw.appBufAvailable() < msgRx_.len()) {
        hw.unlock();
        return 0;
    }
    int len = msgRx_.len();
    uint16_t expCrc = msgRx_.crc();
    msgRx_.reset();
    msgRxStartedMs_ = 0;
    if (len > max_len) {
        uint8_t sink[256];
        int left = len;
        while (left > 0)
            left -= readStream(sink, std::min(left, (int)sizeof(sink)));
        hw.unlock();
        err(FrameErrCause::OverLen);
        return -1;
    }
    readStream(out, len);
    bool ok = UtilCrc::crc16(out, len) == expCrc;
    if (ok) {
        // A CRC-validated delivery proves the link is
        // actually delivering at the locked baud — clears
        // the peer-baud-mismatch escalation signal. Pinned
        // by PeerBaudMismatchTest.
        locksWithoutRecv_ = 0;
        // Same rationale, different counter: a receive-only
        // peer (pong's loop only recv()s, never sends) gets
        // zero ACK frames, so onAck's recentDiscs_ = 0 never
        // executes — recentDiscs_ has exactly one clear site
        // otherwise, and a receive-only peer has no path to
        // it. Left unclearred, three disconnects inside 10 s
        // trip its own DISC_STORM_THRESHOLD veto and force a
        // P1 walk at exactly the moment the master fast-paths
        // P3 at the proven baud — guaranteed miss. A
        // CRC-valid delivery is the same baud-is-good
        // evidence as the ACK case. Pinned by the receive-
        // only-slave two-node itest under itest/recovery/.
        recentDiscs_ = 0;
    }
    hw.unlock();
    if (!ok) {
        err(FrameErrCause::CrcFail);
        return -1;
    }
    return len;
}

#ifdef AUTOLINK_HOST_TEST
bool Link::test_sendMsgBegin(const uint8_t *b, int len) {
    if (cfg.mode != AutoLinkConfig::Mode::SYNC)
        return false;
    if (len < 0 || (size_t)len > cfg.maxMsg)
        return false;
    if (len == 0)
        return true;
    hw.lock();
    if (state != State::OK) {
        hw.unlock();
        return false;
    }
    uint8_t seq = 0;
    buildAndSendMsg_unlocked(b, len, &seq);
    arq_.onSent(seq, NO_BASE, hw.nowMs(), txSeqLap_);
    // Capture the last frame's wire bytes so the host can drive the
    // production ladder on a simulated RTO.
    if (len + MSG_HDR <= MAX_CHUNK) {
        msgHdrEncode((uint32_t)len, UtilCrc::crc16(b, len), testOpBuf_);
        memcpy(testOpBuf_ + MSG_HDR, b, len);
        testOp_.rawLen = MSG_HDR + len;
    } else {
        int lastChunk = len % MAX_CHUNK;
        if (lastChunk == 0)
            lastChunk = MAX_CHUNK;
        memcpy(testOpBuf_, b + (len - lastChunk), lastChunk);
        testOp_.rawLen = lastChunk;
    }
    testOp_.seq = seq;
    testOp_.raw = testOpBuf_;
    testOp_.attempt = 0;
    testOp_.started = false;
    testOp_.firstStepMs = 0;
    hw.unlock();
    return true;
}

bool Link::test_sendMsgStillWaiting() {
    if (cfg.mode != AutoLinkConfig::Mode::SYNC)
        return false;
    hw.lock();
    bool any = arq_.pendingCount() > 0;
    hw.unlock();
    return any;
}

// One ladder step on a simulated RTO; the same step sendMsg uses.
bool Link::test_syncRtoStep() {
    hw.lock();
    arq_.setPending(testOp_.seq, false);
    bool r = syncRtoStep_unlocked(testOp_);
    hw.unlock();
    return r;
}

int Link::test_syncAttempt() const { return testOp_.attempt; }
#endif

} // namespace autolink
