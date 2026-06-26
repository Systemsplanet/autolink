// LinkApi -- the facade methods the AutoLink and host-test
// suites call. Err / clearErr, write / read / peek / available /
// readStream, flush / flushRx / dropLink, sendMsg (the public
// API), recvMsg (the public API), test_sendMsgBegin /
// test_sendMsgStillWaiting (host-test hooks).
//
// sendMsg_unlocked is the chunked COBS frame-build path; it
// lives here because the public sendMsg() is the only caller.
// sendMsg (the public version) handles both SYNC (block-
// inline-for-ACK) and ASYNC (fire-and-forget with ARQ cache)
// modes. recvMsg pulls a single complete message from the
// receive app buffer; on a corrupt MSG_HDR it asks the resync
// helper (LinkRx.cpp) to scan forward for a valid header.
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

int Link::sendMsg_unlocked(const uint8_t *b, int len) {
    if (state != State::OK) {
        Log::log().warning(TAG, "write: not OK -> dropped");
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
    }
    return offset;
}

void Link::dropLink() {
    hw.lock();
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
    rxMsgLen = -1;
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
    hw.lock();
    if (state != State::OK) {
        hw.unlock();
        if (outBaseSeq)
            *outBaseSeq = 0;
        return false;
    }
    if (!sync && !arqCache_.hasRoom()) {
        hw.unlock();
        Log::log().warning(TAG, "sendMsg: ARQ cache full");
        if (outBaseSeq)
            *outBaseSeq = 0;
        return false;
    }
    bool ok = true;
    uint8_t baseSeq = 0;
    if (sync) {
        // SYNC: each frame goes on the
        // wire, the link marks it pending
        // inline, then blocks for the
        // receiver ACK before sending the
        // next. No ARQ cache use, no
        // cobsSeq gaps expected.
        if (len + MSG_HDR <= MAX_CHUNK) {
            uint8_t seq = 0;
            buildAndSendMsg_unlocked(b, len, &seq);
            arq_.onSent(seq, NO_BASE, hw.nowMs());
            if (!arq_.waitForAck(*this, seq, (uint32_t)cfg.syncAckTimeoutMs))
                ok = false;
            baseSeq = seq;
        } else {
            uint16_t c = UtilCrc::crc16(b, len);
            uint8_t hdrOnly[MSG_HDR] = {
                (uint8_t)(len),       (uint8_t)(len >> 8), (uint8_t)(len >> 16),
                (uint8_t)(len >> 24), (uint8_t)(c),        (uint8_t)(c >> 8)
            };
            baseSeq = txSeq;
            sendCobsFrame_unlocked(hdrOnly, MSG_HDR);
            txBytes += 0;
            lastTxMs = hw.nowMs();
            arq_.onSent(baseSeq, NO_BASE, hw.nowMs());
            if (!arq_.waitForAck(*this, baseSeq,
                                 (uint32_t)cfg.syncAckTimeoutMs))
                ok = false;
            int offset = 0;
            while (offset < len && ok) {
                if (state != State::OK) {
                    ok = false;
                    break;
                }
                int chunk = std::min(len - offset, MAX_CHUNK);
                uint8_t seq = txSeq;
                sendCobsFrame_unlocked(b + offset, chunk);
                txBytes += chunk;
                lastTxMs = hw.nowMs();
                arq_.onSent(seq, NO_BASE, hw.nowMs());
                if (!arq_.waitForAck(*this, seq,
                                     (uint32_t)cfg.syncAckTimeoutMs))
                    ok = false;
                offset += chunk;
            }
        }
    } else {
        // ASYNC: each frame goes on the
        // wire and into the ARQ cache
        // (for retransmit on NAK / ACK
        // timeout). Short msg = hdr+payload
        // coalesced into one cache entry;
        // long msg = hdr frame + per-chunk
        // data frames all sharing one
        // baseSeq.
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
                int chunk = std::min(len - offset, MAX_CHUNK);
                sendCobsFrameAcked_unlocked(b + offset, chunk, baseSeq);
                txBytes += chunk;
                lastTxMs = hw.nowMs();
                offset += chunk;
            }
        }
    }
    hw.unlock();
    if (outBaseSeq)
        *outBaseSeq = baseSeq;
#ifdef ARDUINO
    portYIELD();
#endif
    return ok;
}

int Link::recvMsg(uint8_t *out, int max_len) {
    hw.lock();
    if (rxMsgLen < 0) {
        if (hw.appBufAvailable() < MSG_HDR) {
            hw.unlock();
            return 0;
        }
        uint8_t h[MSG_HDR];
        readStream(h, MSG_HDR);
        uint32_t L = (uint32_t)h[0] | ((uint32_t)h[1] << 8) |
            ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
        rxMsgCrc = (uint16_t)h[4] | ((uint16_t)h[5] << 8);
        if (L == 0 || L > cfg.maxMsg) {
            int drop = findMsgHeaderResync_unlocked(cfg.maxMsg + MSG_HDR);
            hw.unlock();
            if (drop > 0)
                err();
            return -1;
        }
        rxMsgLen = (int)L;
    }
    if (hw.appBufAvailable() < rxMsgLen) {
        hw.unlock();
        return 0;
    }
    int len = rxMsgLen;
    uint16_t expCrc = rxMsgCrc;
    rxMsgLen = -1;
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

} // namespace autolink
