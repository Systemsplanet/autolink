
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/util/Log.h"
#include "al/util/UtilCrc.h"
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

void Link::err() {
    hw.lock();
    bool trigger = err_unlocked();
    hw.unlock();
    if (trigger)
        hw.sendBreak();
}

bool Link::err_unlocked() {
    if (state != State::OK)
        return false;
    errs++;
    frameErrs++;
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
    if (cfg.postLockQuietMs <= 0 || lockedAtMs_ == 0 || recentDiscs_ <= 0)
        return false;
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
bool Link::syncRtoStep_unlocked(SyncOp &op) {
    if (op.attempt >= (int)cfg.maxRetx)
        return false;
    op.attempt++;
    buildAndTxCobsFrame_unlocked(op.seq, op.raw, op.rawLen);
    arq_.onSent(op.seq, NO_BASE, hw.nowMs());
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
    rxSeqSet = false;
    rxSeq = 0;
    hw.unlock();
    hw.flushRxHw();
}

bool Link::sendMsg(const uint8_t *b, int len, uint8_t *outBaseSeq) {
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
        hw.unlock();
        Log::log().warning(TAG,
                           "sendMsg: GBN window full (inflight=%d + "
                           "chunks=%d > window=%d) — drop",
                           inflight, chunks, window);
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
            sendCobsFrame_unlocked(merged, MSG_HDR + len);
            txBytes += len;
            lastTxMs = hw.nowMs();
            arq_.onSent(op.seq, NO_BASE, hw.nowMs());
            if (!syncAwaitAcked_unlocked(op)) {
                ok = false;
                brk = onSyncAckTimeout_unlocked(false);
            }
            baseSeq = op.seq;
        } else {
            uint8_t hdrOnly[MSG_HDR];
            msgHdrEncode((uint32_t)len, UtilCrc::crc16(b, len), hdrOnly);
            SyncOp hop;
            hop.seq = txSeq;
            hop.raw = hdrOnly;
            hop.rawLen = MSG_HDR;
            baseSeq = hop.seq;
            sendCobsFrame_unlocked(hdrOnly, MSG_HDR);
            lastTxMs = hw.nowMs();
            arq_.onSent(hop.seq, NO_BASE, hw.nowMs());
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
                sendCobsFrame_unlocked(b + offset, chunk);
                txBytes += chunk;
                lastTxMs = hw.nowMs();
                arq_.onSent(cop.seq, NO_BASE, hw.nowMs());
                if (!syncAwaitAcked_unlocked(cop)) {
                    ok = false;
                    lastSendMsgReason_ = SendMsgReason::SyncMidMessageTimeout;
                    brk = onSyncAckTimeout_unlocked(true);
                }
                offset += chunk;
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
            baseSeq =
                sendCobsFrameAcked_unlocked(merged, MSG_HDR + len, NO_BASE);
            txBytes += len;
            lastTxMs = hw.nowMs();
        } else {
            uint16_t c = UtilCrc::crc16(b, len);
            uint8_t hdrOnly[MSG_HDR] = {
                (uint8_t)(len),       (uint8_t)(len >> 8), (uint8_t)(len >> 16),
                (uint8_t)(len >> 24), (uint8_t)(c),        (uint8_t)(c >> 8)
            };
            baseSeq = sendCobsFrameAcked_unlocked(hdrOnly, MSG_HDR, NO_BASE);
            int offset = 0;
            while (offset < len && ok) {
                if (state != State::OK) {
                    ok = false;
                    break;
                }

                if (!arqCache_.hasRoom()) {
                    noteTxReject_unlocked();
                    Log::log().warning(
                        TAG,
                        "sendMsg: ARQ cache exhausted mid-message "
                        "(emitted %d/%d bytes) — partial send",
                        offset, len);
                    lastSendMsgReason_ = SendMsgReason::PoolExhaust;
                    ok = false;
                    break;
                }
                int chunk = std::min(len - offset, MAX_CHUNK);
                sendCobsFrameAcked_unlocked(b + offset, chunk, baseSeq);
                txBytes += chunk;
                lastTxMs = hw.nowMs();
                offset += chunk;
                // delayUs, not delayMs: a ms-level sleep rounds up
                // to the FreeRTOS tick (10 ms @ 100 Hz) and would
                // cost 10x the ASYNC throughput.
                if (offset < len) {
                    int gap = interChunkGapMs_unlocked();
                    if (gap > 0)
                        hw.delayUs((uint32_t)gap * 1000u);
                }
            }
        }
    }
    if (ok)
        txRejFirstMs_ = txRejLastMs_ = 0;
    // Stamp the success reason before unlock so the app
    // can read lastSendMsgReasonForTest() after sendMsg
    // returns without racing the link thread. Pinned by
    // SendMsgReasonEnumTest.
    lastSendMsgReason_ = ok ? SendMsgReason::Ok : SendMsgReason::NotOk;
    hw.unlock();

    // Wire op after the single unlock (onTimer discipline).
    if (brk)
        hw.sendBreak();
    if (outBaseSeq)
        *outBaseSeq = baseSeq;
#ifdef ARDUINO
    portYIELD();
#endif
    return ok;
}

int Link::recvMsg(uint8_t *out, int max_len) {
    hw.lock();
    if (!msgRx_.inMsg()) {
        if (hw.appBufAvailable() < MSG_HDR) {
            hw.unlock();
            return 0;
        }
        uint8_t h[MSG_HDR];
        readStream(h, MSG_HDR);
        if (!msgRx_.beginMsg(h, cfg.maxMsg)) {
            int drop = findMsgHeaderResync_unlocked(cfg.maxMsg + MSG_HDR);
            // The prior shape only called err() on drop > 0,
            // which undercounted frame errors: a resync scan
            // that returns 0 or -1 is a genuine error (beginMsg
            // already failed) and must count the same way
            // any other frame-error path does. Otherwise the
            // errThreshold / errRateWindow paths never see
            // the bad-header case and a real CRC/desync
            // stream keeps the link up. Pinned by
            // ResyncScanErrTest.
            // The prior shape only called err() on drop > 0,
            // which undercounted frame errors: a resync scan
            // that returns 0 or -1 is a genuine error (beginMsg
            // already failed) and must count the same way
            // any other frame-error path does. Otherwise the
            // errThreshold / errRateWindow paths never see
            // the bad-header case and a real CRC/desync
            // stream keeps the link up. Pinned by
            // ResyncScanErrTest.
            (void)drop;
            hw.unlock();
            err();
            return -1;
        }
    }
    if (hw.appBufAvailable() < msgRx_.len()) {
        hw.unlock();
        return 0;
    }
    int len = msgRx_.len();
    uint16_t expCrc = msgRx_.crc();
    msgRx_.reset();
    if (len > max_len) {
        uint8_t sink[256];
        int left = len;
        while (left > 0)
            left -= readStream(sink, std::min(left, (int)sizeof(sink)));
        hw.unlock();
        err();
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
        err();
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
    arq_.onSent(seq, NO_BASE, hw.nowMs());
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
