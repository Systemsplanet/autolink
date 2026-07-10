
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
        reset_unlocked(true);
        return true;
    }
    if (cfg.errRateWindow > 0 && errWindowCount_ > cfg.errRateWindow) {
        Log::log().warning(TAG, "err rate -> resweep");
        reset_unlocked(true);
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
bool Link::txQuiet_unlocked() const {
    if (cfg.postLockQuietMs <= 0 || lockedAtMs_ == 0 || recentDiscs_ <= 0)
        return false;
    int m = recentDiscs_ > 4 ? 4 : recentDiscs_;
    uint32_t quiet = (uint32_t)cfg.postLockQuietMs * (uint32_t)m;
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
    reset_unlocked(true);
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
    reset_unlocked(true);
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
        return true;
    }
    if (len < 0 || (size_t)len > cfg.maxMsg) {
        if (outBaseSeq)
            *outBaseSeq = 0;
        return false;
    }
    const bool sync = (cfg.mode == AutoLinkConfig::Mode::SYNC);

    const int chunks = chunksForMsgLen(len);
    if (chunks <= 0 || chunks > COBS_SEQ_SPACE) {
        if (outBaseSeq)
            *outBaseSeq = 0;
        return false;
    }
    hw.lock();
    if (state != State::OK) {
        hw.unlock();
        if (outBaseSeq)
            *outBaseSeq = 0;
        return false;
    }
    if (txQuiet_unlocked()) {
        hw.unlock();
        Log::log().debug(TAG, "sendMsg: post-lock quiet -> deferred");
        if (outBaseSeq)
            *outBaseSeq = 0;
        return false;
    }

    // GBN admission: the sender's in-flight set is always a
    // contiguous window (cumulative ACK only advances the base),
    // so one bound replaces the old seq-space + cache-room +
    // stalled-span gates: inflight + this message's chunks must
    // fit the pipeline window. The cache pool (2x window) always
    // has room whenever this holds, so a separate cache-full check
    // can't fire first.
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
        return false;
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
                    ok = false;
                    break;
                }
                int chunk = std::min(len - offset, MAX_CHUNK);
                sendCobsFrameAcked_unlocked(b + offset, chunk, baseSeq);
                txBytes += chunk;
                lastTxMs = hw.nowMs();
                offset += chunk;
                // Inter-chunk gap: inserted between successive
                // data chunks of the same multi-chunk message
                // (NOT between messages), only in ASYNC mode.
                // 0 ms gap (the default for SYNC, or when the
                // app sets asyncChunkGapMs=0) is a no-op here.
                // delayUs (microsecond busy-wait) is the
                // sub-tick primitive — a ms-level delay would
                // round up to the FreeRTOS tick (10 ms @ 100 Hz)
                // and 10x the ASYNC throughput.
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
            hw.unlock();
            if (drop > 0)
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
    // Capture the last frame's wire bytes so test_syncRtoStep can
    // exercise the production ladder on a simulated RTO. Small
    // messages ride one merged frame; large ones end on a raw chunk.
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

// One ladder step on a simulated RTO expiry; drives the SAME
// production step the blocking sendMsg loop uses.
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
