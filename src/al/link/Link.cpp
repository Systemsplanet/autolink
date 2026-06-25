// Wire-protocol implementation.
// Pure decisions in LinkDecision.h.
#include "al/link/Link.h"
#include "al/link/LinkDecision.h"
#include "al/util/Log.h"
#include <cstdio>

#ifdef ARDUINO
#    if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#        include <freertos/FreeRTOS.h>
#    endif
#endif
#include "al/util/UtilCrc.h"
#include "al/util/UtilCobs.h"
#include <algorithm>
#include <string.h>

static constexpr const char *TAG = "AutoLink";

namespace autolink
{
static_assert(MAX_CHUNK + 6 <= 256, "MAX_CHUNK too large");

static constexpr int PHASE1_MAX_TRIES = 6;
static constexpr int PHASE3_ACKS_NEEDED = 2;
static constexpr int HEARTBEAT_MS = 100;
static constexpr int HEARTBEAT_MISS_LIMIT = 3;
// Asymmetric idle: TX active, RX silent
// → peer gone, drop fast.
static constexpr int FAST_IDLE_RX_MS = 300;
static constexpr int FAST_IDLE_TX_MS = 1000;

int Link::okTickMs() const
{
    int k = cfg.idleTimeoutMs / 3;
    if (k < 50)
        k = 50;
    return k < (int)ACK_RTO_MS ? (int)ACK_RTO_MS : k;
}

int Link::phase1ArmMs()
{
#ifdef AUTOLINK_HOST_TEST
    return dwells_.phase1;
#else
    return jitterPhase1Dwell(dwells_.phase1,
                             hw.nowMs() ^ (isMaster ? 0xA5u : 0x5Au));
#endif
}

const char *StateToStr(State s)
{
    switch (s) {
    case State::OK:
        return "OK";
    case State::SWP:
        return "SWP";
    case State::LCK:
        return "LCK";
    default:
        return "UNK";
    }
}

Link::Link(IHal &h, bool isMasterNode, const AutoLinkConfig &config)
    : hw(h), isMaster(isMasterNode), cfg(config), state(State::OK), errs(0),
      spdI(0), pingSample(0), emptySweeps(0),
      baudSweep((int)config.allowedBaudsCount), rxIdx(0), frameRx(*this),
      rxMsgLen(-1), rxMsgCrc(0), lckRetries(0), lastRxMs(0), lastTxMs(0),
      txBytes(0), rxBytes(0), discCount(0), frameErrs(0)
{
    UtilBaudSweep::Config sc;
    sc.pingSamplesPerBaud = config.pingSamplesPerBaud;
    sc.minAcceptRate = config.minAcceptRate;
    sc.expectedSamples = -1;
    baudSweep.configure(sc);
    hw.bind(this);
    Log::log().info(TAG, "Init as %s", isMaster ? "Ping" : "Pong");
    if (cfg.maxMsg > cfg.streamBufferSize)
        Log::log().error(TAG,
                         "maxMsg > streamBufSize: "
                         "large msgs will be dropped");
}

Link::~Link()
{
    for (int i = 0; i < 256; i++) {
        if (reorder_[i].buf)
            free(reorder_[i].buf);
        reorder_[i].buf = nullptr;
        reorder_[i].in_use = false;
        reorder_[i].len = 0;
    }
}

void Link::reorderClear_unlocked()
{
    for (int i = 0; i < 256; i++) {
        if (reorder_[i].buf)
            free(reorder_[i].buf);
        reorder_[i].buf = nullptr;
        reorder_[i].in_use = false;
        reorder_[i].len = 0;
    }
}

void Link::reorderDropExpired_unlocked(uint32_t nowMs)
{
    for (int i = 0; i < 256; i++) {
        if (!reorder_[i].in_use)
            continue;
        uint32_t age = nowMs - reorder_[i].heldAtMs;
        if (age < (uint32_t)cfg.reorderHoldMs)
            continue;
        free(reorder_[i].buf);
        reorder_[i].buf = nullptr;
        reorder_[i].in_use = false;
        reorder_[i].len = 0;
        lostMsgs++;
        Log::log().warning(TAG,
                           "reorder seq=%u expired "
                           "(%ums)",
                           (unsigned)i, (unsigned)age);
    }
}

int Link::reorderFlushContiguous_unlocked(uint32_t)
{
    int delivered = 0;
    bool progress = true;
    while (progress) {
        progress = false;
        uint8_t exp =
            (uint8_t)((rxSeq == (uint8_t)COBS_SEQ_MAX) ? 0 : rxSeq + 1);
        if (!reorder_[exp].in_use)
            break;
        ReorderSlot &s = reorder_[exp];
        int acc = hw.pushAppBuf(s.buf, s.len);
        if (acc < s.len) {
            Log::log().info(TAG,
                            "reorder flush seq=%u "
                            "buf full (want %d got %d)",
                            (unsigned)exp, (int)s.len, acc);
            break;
        }
        sendAckFrame_unlocked(exp);
        rxSeq = exp;
        rxSeqSet = true;
        rxBytes += s.len;
        free(s.buf);
        s.buf = nullptr;
        s.in_use = false;
        s.len = 0;
        delivered++;
        progress = true;
    }
    if (delivered > 0 && errs > 0)
        errs = 0;
    return delivered;
}

void Link::resetSeq_unlocked()
{
    txSeq = 0;
    rxSeqSet = false;
    rxSeq = 0;
}

void Link::computeDwells_unlocked()
{
    int N = cfg.allowedBaudsCount;
    int maxN = (int)(sizeof(dwells_.phase2) / sizeof(dwells_.phase2[0]));
    if (N > maxN)
        N = maxN;
    for (int i = 0; i < N; i++) {
        double rt = 2.0 * (5.0 * 10.0 / cfg.allowedBauds[i] * 1000.0) + 0.5;
        int d = (int)(rt * 1.5) + 1;
        if (d < 5)
            d = 5;
        dwells_.phase2[i] = d;
        dwells_.phase2Slave[i] = d;
    }
    double rt0 = 2.0 * (5.0 * 10.0 / cfg.allowedBauds[0] * 1000.0) + 0.5;
    dwells_.phase3 = (int)(3.0 * rt0 * 1.5) + 1;
    dwells_.phase1 = cfg.delayMs;
    int total = 0;
    for (int i = 0; i < N; i++)
        total += dwells_.phase2[i];
    dwells_.phase2Total = total * 5 + 200;
    for (int i = 0; i < N; i++) {
        if (dwells_.phase2[i] < 5)
            dwells_.phase2[i] = 5;
        if (dwells_.phase2Slave[i] < 5)
            dwells_.phase2Slave[i] = 5;
    }
}

void Link::begin()
{
    hw.lock();
    computeDwells_unlocked();
    hw.unlock();
    if (isMaster) {
        hw.lock();
        reset_unlocked(false);
        enterPhase1_unlocked();
        hw.unlock();
        hw.sendBreak();
    } else {
        hw.lock();
        changeState_unlocked(State::SWP);
        spdI = cfg.allowedBaudsCount - 1;
        pingSample = 0;
        rxIdx = 0;
        rxMsgLen = -1;
        frameRx.reset();
        baudSweep.resetAll();
        resetSeq_unlocked();
        sweepPhase_ = SweepPhase::PHASE1;
        hw.unlock();
        hw.clearAppBuf();
        hw.setSpd(cfg.allowedBauds[spdI]);
        Log::log().info(TAG, "SWP Pong P1 baud[%d]=%lu", spdI,
                        (unsigned long)cfg.allowedBauds[spdI]);
        hw.startTimer(cfg.pingSamplesPerBaud * cfg.delayMs);
    }
}

void Link::changeState_unlocked(State newState)
{
    if (state != newState) {
        Log::log().debug(TAG, "%s -> %s", StateToStr(state),
                         StateToStr(newState));
        state = newState;
    }
}

int Link::bestSpd_unlocked() const
{
    int best = baudSweep.pickBest();
    if (best < 0)
        return 0;
    for (int j = 0; j < best; j++)
        if (baudSweep.scoreAt(j) > 0)
            return j;
    return best;
}

void Link::sendPongAck_unlocked()
{
    sendFrame_unlocked(PONG_CMD);
}

void Link::enterPhase1_unlocked()
{
    // Never leave P1 until connected.
    sweepPhase_ = SweepPhase::PHASE1;
    spdI = cfg.allowedBaudsCount - 1;
    pingSample = 0;
    hw.setSpd(cfg.allowedBauds[spdI]);
    Log::log().info(TAG, "=== P1 slowest baud[%d]=%lu ===", spdI,
                    (unsigned long)cfg.allowedBauds[spdI]);
    if (isMaster) {
        sendFrame_unlocked(PING_CMD);
        hw.startTimer(dwells_.phase1);
    } else {
        hw.startTimer(dwells_.phase1 * PHASE1_MAX_TRIES);
    }
}

void Link::enterPhase2_unlocked()
{
    sweepPhase_ = SweepPhase::PHASE2;
    spdI = 0;
    pingSample = 0;
    hw.setSpd(cfg.allowedBauds[0]);
    Log::log().info(TAG, "=== P2 top-down sweep ===");
    if (isMaster) {
        sendFrame_unlocked(PING_CMD);
        hw.startTimer(dwells_.phase2[0]);
    }
}

void Link::enterPhase3_unlocked(int chosenBaud)
{
    sweepPhase_ = SweepPhase::PHASE3;
    phase3Baud_ = chosenBaud;
    phase3Acks_ = 0;
    hw.setSpd(cfg.allowedBauds[chosenBaud]);
    Log::log().info(TAG, "=== P3 2-of-3 baud[%d]=%lu ===", chosenBaud,
                    (unsigned long)cfg.allowedBauds[chosenBaud]);
    if (isMaster) {
        sendFrame_unlocked(PING_CMD);
        int rt =
            (int)(2.0 * (5.0 * 10.0 / cfg.allowedBauds[chosenBaud] * 1000.0) +
                  0.5);
        if (rt < 5)
            rt = 5;
        int t3 = rt * (PHASE3_ACKS_NEEDED + 1) + 100;
        if (t3 < 200)
            t3 = 200;
        hw.startTimer(t3);
    }
}

void Link::enterResweep_unlocked()
{
    sweepPhase_ = SweepPhase::PHASE2;
    if (cfg.baudPreference && preferredBaud_ != NO_PREFERRED_BAUD &&
        preferredBaud_ < (int)cfg.allowedBaudsCount) {
        if (baudRetries_ < cfg.baudRetryLimit) {
            spdI = preferredBaud_;
            baudRetries_++;
        } else {
            spdI = 0;
            preferredBaud_ = NO_PREFERRED_BAUD;
            baudRetries_ = 0;
        }
    } else {
        spdI = 0;
    }
    pingSample = 0;
    hw.setSpd(cfg.allowedBauds[spdI]);
    hw.startTimer(isMaster ? dwells_.phase2[spdI] : dwells_.phase2Slave[spdI]);
}

void Link::sendFrame_unlocked(uint8_t payload)
{
    uint8_t frame[CTRL_FRAME_SIZE];
    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[CTRL_FRAME_SEQ_IDX] = txSeq;
    frame[CTRL_FRAME_PAYLOAD_IDX] = payload;
    frame[CTRL_FRAME_CRC_IDX] = UtilCrc::crc8(frame, CTRL_FRAME_SIZE - 1);
    if (hw.tx(frame, CTRL_FRAME_SIZE) != CTRL_FRAME_SIZE)
        Log::log().error(TAG, "sendFrame truncated");
}

void Link::sendFrame(uint8_t payload)
{
    hw.lock();
    sendFrame_unlocked(payload);
    hw.unlock();
}

void Link::sendCobsFrame_unlocked(const uint8_t *b, int n)
{
    if (n < 0)
        n = 0;
    if (n > MAX_CHUNK)
        n = MAX_CHUNK;
    uint8_t unenc[MAX_CHUNK + 3];
    unenc[0] = txSeq;
    if (n > 0)
        memcpy(unenc + 1, b, n);
    unenc[1 + n] = UtilCrc::crc8(unenc, 1 + n);
    size_t rawLen = (size_t)(1 + n) + 1;
    uint8_t frame[MAX_CHUNK + 6];
    frame[0] = 0x00;
    size_t encLen = UtilCobs::encode(unenc, rawLen, frame + 1);
    frame[1 + encLen] = 0x00;
    hw.tx(frame, (int)(encLen + 2));
    // Skip 0xFE/0xFF: wire discriminators.
    txSeq = (txSeq == COBS_SEQ_MAX) ? 0 : (uint8_t)(txSeq + 1);
}

void Link::sendCobsFrame(const uint8_t *b, int n)
{
    hw.lock();
    sendCobsFrame_unlocked(b, n);
    hw.unlock();
}

uint8_t Link::sendCobsFrameAcked_unlocked(const uint8_t *b, int n,
                                          uint8_t baseSeq)
{
    uint8_t seq = txSeq;
    sendCobsFrame_unlocked(b, n);
    ackedPending_[seq] = true;
    retxCount_[seq] = 0;
    sentAtMs_[seq] = hw.nowMs();
    baseSeq_[seq] = (baseSeq == NO_BASE) ? seq : baseSeq;
    if (arqCacheInsertCallback_ && n > 0)
        arqCacheInsertCallback_(seq, b, n, 1, arqCtx_);
    return seq;
}

void Link::resendCobsFrame_unlocked(uint8_t seq, const uint8_t *b, int n)
{
    if (n < 0)
        n = 0;
    if (n > MAX_CHUNK)
        n = MAX_CHUNK;
    uint8_t unenc[MAX_CHUNK + 3];
    unenc[0] = seq;
    if (n > 0)
        memcpy(unenc + 1, b, n);
    unenc[1 + n] = UtilCrc::crc8(unenc, 1 + n);
    size_t rawLen = (size_t)(1 + n) + 1;
    uint8_t frame[MAX_CHUNK + 6];
    frame[0] = 0x00;
    size_t encLen = UtilCobs::encode(unenc, rawLen, frame + 1);
    frame[1 + encLen] = 0x00;
    hw.tx(frame, (int)(encLen + 2));
}

void Link::sendAckFrame_unlocked(uint8_t ackedCobsSeq)
{
    uint8_t u[3] = { ACK_TYPE, ackedCobsSeq, 0 };
    u[2] = UtilCrc::crc8(u, 2);
    uint8_t frame[8];
    frame[0] = 0x00;
    size_t el = UtilCobs::encode(u, 3, frame + 1);
    frame[1 + el] = 0x00;
    hw.tx(frame, (int)(el + 2));
}

void Link::sendNakFrame_unlocked(uint8_t missingCobsSeq)
{
    uint8_t u[3] = { NAK_TYPE, missingCobsSeq, 0 };
    u[2] = UtilCrc::crc8(u, 2);
    uint8_t frame[8];
    frame[0] = 0x00;
    size_t el = UtilCobs::encode(u, 3, frame + 1);
    frame[1 + el] = 0x00;
    hw.tx(frame, (int)(el + 2));
}

void Link::err()
{
    hw.lock();
    bool trigger = err_unlocked();
    hw.unlock();
    if (trigger)
        hw.sendBreak();
}

bool Link::err_unlocked()
{
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

void Link::clearErr()
{
    hw.lock();
    if (errs > 0)
        errs = 0;
    hw.unlock();
}

int Link::available() const
{
    return hw.appBufAvailable();
}
int Link::peek()
{
    return hw.peekAppBuf();
}
int Link::read()
{
    uint8_t b;
    return hw.popAppBuf(&b, 1) == 1 ? b : -1;
}
int Link::read(uint8_t *b, int max_len)
{
    return hw.popAppBuf(b, max_len);
}

int Link::readStream(uint8_t *b, int n)
{
    int got = 0;
    while (got < n) {
        int r = hw.popAppBuf(b + got, n - got);
        if (r <= 0)
            break;
        got += r;
    }
    return got;
}

int Link::write(const uint8_t *b, int len)
{
    if (len <= 0)
        return 0;
    hw.lock();
    int sent = sendMsg_unlocked(b, len);
    hw.unlock();
    return sent;
}

int Link::sendMsg_unlocked(const uint8_t *b, int len)
{
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

void Link::dropLink()
{
    hw.lock();
    reset_unlocked(true);
    bool nb = (state == State::SWP);
    hw.unlock();
    if (nb)
        hw.sendBreak();
}

void Link::flush()
{
    hw.flushTx();
}

void Link::flushRx()
{
    hw.lock();
    hw.clearAppBuf();
    rxMsgLen = -1;
    rxSeqSet = false;
    rxSeq = 0;
    hw.unlock();
    hw.flushRxHw();
}

bool Link::sendMsg(const uint8_t *b, int len, uint8_t *outBaseSeq)
{
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
    uint16_t c = UtilCrc::crc16(b, len);
    uint8_t hdr[MSG_HDR] = { (uint8_t)(len),       (uint8_t)(len >> 8),
                             (uint8_t)(len >> 16), (uint8_t)(len >> 24),
                             (uint8_t)(c),         (uint8_t)(c >> 8) };
    const bool sync = (cfg.mode == AutoLinkConfig::Mode::SYNC);
    hw.lock();
    if (state != State::OK) {
        hw.unlock();
        if (outBaseSeq)
            *outBaseSeq = 0;
        return false;
    }
    if (arqCacheHasRoomCallback_ && !arqCacheHasRoomCallback_(arqCtx_)) {
        hw.unlock();
        Log::log().warning(TAG, "sendMsg: ARQ cache full");
        if (outBaseSeq)
            *outBaseSeq = 0;
        return false;
    }
    bool ok = true;
    uint8_t baseSeq = 0;
    auto waitSyncAck = [&](uint8_t seq) -> bool {
        // Caller has the lock. Drop it so
        // the link task can deliver the ACK,
        // then poll until acked or timeout.
        hw.unlock();
        uint32_t t0 = hw.nowMs();
        while (ackedPending_[seq]) {
            if ((hw.nowMs() - t0) >= (uint32_t)cfg.syncAckTimeoutMs) {
                Log::log().warning(TAG, "SYNC: seq=%u ack timeout",
                                   (unsigned)seq);
                hw.lock();
                ackedPending_[seq] = false;
                retxCount_[seq] = 0;
                return false;
            }
#ifdef ARDUINO
            portYIELD();
#endif
        }
        hw.lock();
        return true;
    };
    if (len + MSG_HDR <= MAX_CHUNK) {
        // Short msg: coalesce hdr+data
        // into one wire frame.
        uint8_t merged[MAX_CHUNK];
        memcpy(merged, hdr, MSG_HDR);
        memcpy(merged + MSG_HDR, b, len);
        if (sync) {
            // SYNC: send raw, no ARQ cache
            // insert, wait for ACK inline.
            baseSeq = txSeq;
            sendCobsFrame_unlocked(merged, MSG_HDR + len);
            txBytes += len;
            lastTxMs = hw.nowMs();
            ackedPending_[baseSeq] = true;
            retxCount_[baseSeq] = 0;
            if (!waitSyncAck(baseSeq))
                ok = false;
        } else {
            // ASYNC: ARQ cache insert +
            // async retransmit machinery.
            baseSeq =
                sendCobsFrameAcked_unlocked(merged, MSG_HDR + len, NO_BASE);
            txBytes += len;
            lastTxMs = hw.nowMs();
        }
    } else {
        if (sync) {
            baseSeq = txSeq;
            sendCobsFrame_unlocked(hdr, MSG_HDR);
            txBytes += 0;
            lastTxMs = hw.nowMs();
            ackedPending_[baseSeq] = true;
            retxCount_[baseSeq] = 0;
            if (!waitSyncAck(baseSeq))
                ok = false;
        } else {
            baseSeq = sendCobsFrameAcked_unlocked(hdr, MSG_HDR, NO_BASE);
        }
        int offset = 0;
        while (offset < len && ok) {
            if (state != State::OK) {
                ok = false;
                break;
            }
            int chunk = std::min(len - offset, MAX_CHUNK);
            if (sync) {
                uint8_t seq = txSeq;
                sendCobsFrame_unlocked(b + offset, chunk);
                txBytes += chunk;
                lastTxMs = hw.nowMs();
                ackedPending_[seq] = true;
                retxCount_[seq] = 0;
                if (!waitSyncAck(seq))
                    ok = false;
            } else {
                sendCobsFrameAcked_unlocked(b + offset, chunk, baseSeq);
                txBytes += chunk;
                lastTxMs = hw.nowMs();
            }
            offset += chunk;
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

int Link::findMsgHeaderResync_unlocked(int max_scan)
{
    int avail = hw.appBufAvailable();
    if (avail < MSG_HDR)
        return -1;
    int scan = (max_scan > 0 && max_scan < avail) ? max_scan : avail;
    int snapLen = scan + MSG_HDR;
    if (snapLen > avail)
        snapLen = avail;
    uint8_t *snap = (uint8_t *)malloc(snapLen);
    if (!snap)
        return -1;
    int got = hw.popAppBuf(snap, snapLen);
    for (int d = 0; d + MSG_HDR <= got; d++) {
        uint32_t L = (uint32_t)snap[d] | ((uint32_t)snap[d + 1] << 8) |
            ((uint32_t)snap[d + 2] << 16) | ((uint32_t)snap[d + 3] << 24);
        if (L < 1 || L > cfg.maxMsg)
            continue;
        if (d + (int)L + 6 > got)
            continue;
        uint16_t wc = (uint16_t)snap[d + 4] | ((uint16_t)snap[d + 5] << 8);
        if (UtilCrc::crc16(snap + d + 6, (int)L) != wc)
            continue;
        hw.pushAppBuf(snap + d, got - d);
        free(snap);
        return d;
    }
    // No valid header found in the scan
    // window. The buffer is unrecoverable
    // garbage — drop everything, not just
    // the snap. Pushing the snap back
    // keeps the desync alive: every
    // subsequent recvMsg() call would
    // re-scan the same noise and re-fail,
    // and any bytes that were in the
    // buffer beyond the scan window would
    // still be there waiting to confuse
    // the next valid frame. Drain the
    // whole appBuf so the next valid
    // frame on the wire (which the peer's
    // retransmit will produce) has a
    // clean buffer to land in.
    free(snap);
    hw.clearAppBuf();
    return -1;
}

int Link::recvMsg(uint8_t *out, int max_len)
{
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

void Link::getStats(Stats &s) const
{
    hw.lock();
    s.tx = txBytes;
    s.rx = rxBytes;
    s.discCount = discCount;
    s.frameErrs = frameErrs;
    hw.unlock();
}
void Link::resetStats()
{
    hw.lock();
    txBytes = rxBytes = 0;
    hw.unlock();
}
void Link::resetErrors()
{
    hw.lock();
    discCount = frameErrs = 0;
    errWindowCount_ = 0;
    errWindowStartMs_ = hw.nowMs();
    hw.unlock();
}
void Link::resetDiag()
{
    hw.lock();
    gaps = stale = lostMsgs = 0;
    hw.unlock();
}

State Link::getState() const
{
    hw.lock();
    State s = state;
    hw.unlock();
    return s;
}
int Link::getErrCount() const
{
    hw.lock();
    int e = errs;
    hw.unlock();
    return e;
}
int Link::getCurrentSpdIndex() const
{
    hw.lock();
    int i = spdI;
    hw.unlock();
    return i;
}
uint32_t Link::getCurrentBaud() const
{
    hw.lock();
    uint32_t b = (spdI >= 0 && spdI < (int)cfg.allowedBaudsCount)
        ? cfg.allowedBauds[spdI]
        : 0;
    hw.unlock();
    return b;
}
void Link::getDiag(Diag &d) const
{
    hw.lock();
    d.txSeq = txSeq;
    d.rxSeqSet = rxSeqSet;
    d.rxSeq = rxSeq;
    d.gaps = gaps;
    d.stale = stale;
    d.lostMsgs = lostMsgs;
    d.baudRetries = (uint64_t)baudRetries_;
    d.preferredBaud = preferredBaud_;
    hw.unlock();
}

void Link::lockOk_unlocked(int idx, const char *tag)
{
    // Record preferred baud so reset can
    // start next sweep there.
    hw.setSpd(cfg.allowedBauds[idx]);
    spdI = idx;
    errs = 0;
    preferredBaud_ = (uint8_t)idx;
    baudRetries_ = 0;
    errWindowStartMs_ = hw.nowMs();
    errWindowCount_ = 0;
    heartbeatPingsMissed_ = 0;
    lastHeartbeatMs_ = hw.nowMs();
    lastRxMs = lastTxMs = hw.nowMs();
    Log::log().info(TAG, "Locked %lu baud (%s)",
                    (unsigned long)cfg.allowedBauds[idx], tag);
    changeState_unlocked(State::OK);
    wasEverOk_ = true;
    if (cfg.idleTimeoutMs > 0)
        hw.startTimer(okTickMs());
}

void Link::onRx(const uint8_t *data, int len)
{
    hw.lock();
    int i = 0;
    if (state == State::SWP)
        swpRxBytes += len;
    lastRxMs = hw.nowMs();
    bool needBreak = false;
    while (i < len) {
        State cur = state;
        if (cur == State::OK) {
            int start = i;
            while (i < len) {
                uint8_t b = data[i];
                if (b == 0xAA && (len - i) >= 2 && data[i + 1] == 0x55) {
                    if (i > start) {
                        int c = frameRx.feed(data + start, i - start);
                        if (state != State::OK) {
                            i = start + c;
                            break;
                        }
                    }
                    if (len - i >= CTRL_FRAME_SIZE) {
                        for (int k = 0; k < CTRL_FRAME_SIZE; k++)
                            rxBuf[k] = data[i + k];
                        i += CTRL_FRAME_SIZE;
                        rxIdx = 0;
                        if (UtilCrc::crc8(rxBuf, CTRL_FRAME_SIZE - 1) !=
                            rxBuf[CTRL_FRAME_CRC_IDX]) {
                            if (err_unlocked())
                                needBreak = true;
                        } else {
                            uint8_t pl = rxBuf[CTRL_FRAME_PAYLOAD_IDX];
                            if (pl == PING_CMD)
                                sendPongAck_unlocked();
                            else if (pl == PONG_CMD)
                                heartbeatPingsMissed_ = 0;
                        }
                        start = i;
                    } else {
                        int c = frameRx.feed(data + start, len - start);
                        i = start + c;
                        start = i;
                        break;
                    }
                } else {
                    i++;
                }
            }
            if (i > start && state == State::OK) {
                int c = frameRx.feed(data + start, i - start);
                i = start + c;
            }
            if (state != State::OK)
                needBreak = true;
        } else {
            uint8_t b = data[i++];
            if (rxIdx == 0 && b != 0xAA)
                continue;
            if (rxIdx == 1 && b != 0x55) {
                rxIdx = 0;
                continue;
            }
            rxBuf[rxIdx++] = b;
            if (rxIdx == CTRL_FRAME_SIZE) {
                rxIdx = 0;
                if (UtilCrc::crc8(rxBuf, CTRL_FRAME_SIZE - 1) !=
                    rxBuf[CTRL_FRAME_CRC_IDX]) {
                    if (err_unlocked())
                        needBreak = true;
                    continue;
                }
                uint8_t cs = rxBuf[CTRL_FRAME_SEQ_IDX];
                uint8_t pl = rxBuf[CTRL_FRAME_PAYLOAD_IDX];
                if (ctrlFrameReady_unlocked(cs, pl, cur))
                    needBreak = true;
            }
        }
    }
    hw.unlock();
    if (needBreak)
        hw.sendBreak();
}

bool Link::ctrlFrameReady_unlocked(uint8_t cs, uint8_t pl, State cur)
{
    if (cur == State::SWP)
        return handleSwp_unlocked(cs, pl);
    if (cur == State::LCK)
        return handleLck_unlocked(cs, pl);
    return false;
}

bool Link::handleSwp_unlocked(uint8_t cobsSeq, uint8_t payload)
{
    (void)cobsSeq;
    if (!isMaster && payload == REQ_CMD) {
        int best = bestSpd_unlocked();
        sendFrame_unlocked((uint8_t)best);
        lockOk_unlocked(best, "REQ");
        return false;
    }
    if (payload >= LOCK_CMD_BASE &&
        payload < LOCK_CMD_BASE + cfg.allowedBaudsCount) {
        int lb = (int)(payload - LOCK_CMD_BASE);
        hw.setSpd(cfg.allowedBauds[lb]);
        spdI = lb;
        sweepPhase_ = SweepPhase::NONE;
        phase3Baud_ = -1;
        phase3Acks_ = 0;
        lockOk_unlocked(lb,
                        sweepPhase_ == SweepPhase::PHASE3
                            ? (isMaster ? "p3-lock" : "p3-pong")
                            : "lock");
        return false;
    }
    if (payload == PONG_CMD && isMaster) {
        if (sweepPhase_ == SweepPhase::PHASE1) {
            int lb = spdI;
            sweepPhase_ = SweepPhase::NONE;
            phase3Baud_ = -1;
            phase3Acks_ = 0;
            hw.setSpd(cfg.allowedBauds[lb]);
            spdI = lb;
            sendFrame_unlocked(LOCK_CMD + (uint8_t)lb);
            lockOk_unlocked(lb, "phase1");
            return false;
        }
        if (sweepPhase_ == SweepPhase::PHASE2) {
            enterPhase3_unlocked(spdI);
            return false;
        }
        if (sweepPhase_ == SweepPhase::PHASE3) {
            phase3Acks_++;
            if (phase3Acks_ >= PHASE3_ACKS_NEEDED) {
                int lb = phase3Baud_;
                sweepPhase_ = SweepPhase::NONE;
                phase3Baud_ = -1;
                phase3Acks_ = 0;
                hw.setSpd(cfg.allowedBauds[lb]);
                spdI = lb;
                sendFrame_unlocked(LOCK_CMD + (uint8_t)lb);
                lockOk_unlocked(lb, "phase3");
                return false;
            }
            sendFrame_unlocked(PING_CMD);
            int rt = (int)(2.0 *
                               (5.0 * 10.0 / cfg.allowedBauds[phase3Baud_] *
                                1000.0) +
                           0.5);
            if (rt < 5)
                rt = 5;
            int t3 = rt * (PHASE3_ACKS_NEEDED - phase3Acks_ + 1) + 100;
            if (t3 < 200)
                t3 = 200;
            hw.startTimer(t3);
            heartbeatPingsMissed_ = 0;
            return false;
        }
    }
    if (payload == PING_CMD && !isMaster) {
        baudSweep.score(spdI);
        if (sweepPhase_ == SweepPhase::PHASE1) {
            sendPongAck_unlocked();
        } else if (sweepPhase_ == SweepPhase::PHASE2) {
            sweepPhase_ = SweepPhase::PHASE3;
            phase3Baud_ = spdI;
            phase3Acks_ = 0;
            hw.setSpd(cfg.allowedBauds[phase3Baud_]);
        } else if (sweepPhase_ == SweepPhase::PHASE3) {
            phase3Acks_++;
            if (phase3Acks_ >= PHASE3_ACKS_NEEDED) {
                int lb = phase3Baud_;
                sweepPhase_ = SweepPhase::NONE;
                phase3Baud_ = -1;
                phase3Acks_ = 0;
                hw.setSpd(cfg.allowedBauds[lb]);
                spdI = lb;
                sendFrame_unlocked(LOCK_CMD + (uint8_t)lb);
                lockOk_unlocked(lb, "p3-pong");
                return false;
            }
        }
        sendPongAck_unlocked();
        heartbeatPingsMissed_ = 0;
    }
    return false;
}

bool Link::handleLck_unlocked(uint8_t cobsSeq, uint8_t payload)
{
    (void)cobsSeq;
    if (isMaster) {
        if (payload < (int)cfg.allowedBaudsCount)
            lockOk_unlocked((int)payload, "REQ");
        return false;
    }
    if (payload == REQ_CMD) {
        int best = bestSpd_unlocked();
        sendFrame_unlocked((uint8_t)best);
        lockOk_unlocked(best, "REQ");
    }
    return false;
}

void Link::reset_unlocked(bool count)
{
    if (count && state == State::OK)
        discCount++;
    changeState_unlocked(State::SWP);
    // Always restart P1 at slowest baud.
    // Honoring preferredBaud_ risks
    // re-locking on the baud that failed.
    spdI = 0;
    preferredBaud_ = NO_PREFERRED_BAUD;
    baudRetries_ = 0;
    pingSample = 0;
    rxIdx = 0;
    rxMsgLen = -1;
    frameRx.reset();
    baudSweep.resetAll();
    errs = lckRetries = 0;
    emptySweeps = swpRxBytes = 0;
    errWindowStartMs_ = hw.nowMs();
    errWindowCount_ = 0;
    lastRxMs = hw.nowMs();
    memset(ackedPending_, 0, sizeof(ackedPending_));
    memset(retxCount_, 0, sizeof(retxCount_));
    memset(sentAtMs_, 0, sizeof(sentAtMs_));
    memset(baseSeq_, 0, sizeof(baseSeq_));
    hasPendingRetx_ = false;
    pendingRetxBase_ = NO_BASE;
    reorderClear_unlocked();
    resetSeq_unlocked();
    hw.clearAppBuf();
    enterPhase1_unlocked();
    if (linkResetCallback_)
        linkResetCallback_(arqCtx_);
    if (arqCacheClearAllCallback_)
        arqCacheClearAllCallback_(arqCtx_);
}

bool Link::onPayload(uint8_t cobsSeq, const uint8_t *b, int n)
{
    if (state != State::OK)
        return true;
    reorderDropExpired_unlocked(hw.nowMs());
    int diff = 0;
    GapClass cls = classifyGap(cobsSeq, rxSeq, rxSeqSet, &diff);
    if (cls == GapClass::Stale) {
        stale++;
        return false;
    }
    if (cls == GapClass::Gap) {
        if (cfg.mode == AutoLinkConfig::Mode::SYNC) {
            // Sender waits for ack before
            // sending the next frame, so
            // gaps shouldn't happen. If they
            // do, the wire has reordered or
            // duplicated something; drop it
            // and ACK so the sender's pool
            // frees up. No reorder buffer
            // use, no malloc.
            gaps++;
            lostMsgs += (uint64_t)diff;
            if (n > 0) {
                hw.pushAppBuf(b, n);
                rxBytes += n;
            }
            sendAckFrame_unlocked(cobsSeq);
            return false;
        }
        if (cfg._test_forwardResync) {
            gaps++;
            lostMsgs += (uint64_t)diff;
            rxSeq = cobsSeq;
            rxSeqSet = true;
            if (n > 0) {
                hw.pushAppBuf(b, n);
                rxBytes += n;
            }
            sendAckFrame_unlocked(cobsSeq);
            return false;
        }
        uint8_t exp =
            (uint8_t)((rxSeq == (uint8_t)COBS_SEQ_MAX) ? 0 : rxSeq + 1);
        gaps++;
        // Don't bump lostMsgs here. The
        // out-of-order frame is held in
        // the reorder buffer waiting for
        // the missing seq; if the
        // retransmit arrives within
        // reorderHoldMs, the frame is
        // delivered in-order and nothing
        // was lost. lostMsgs is only
        // incremented when the reorder
        // buffer expires (see
        // reorderDropExpired_unlocked).
        Log::log().info(TAG, "GAP seq=%u exp=%u diff=%d", (unsigned)cobsSeq,
                        (unsigned)exp, diff);
        if (n > 0) {
            uint8_t *slotBuf = (uint8_t *)malloc(n);
            if (!slotBuf) {
                lostMsgs++;
                rxSeq = cobsSeq;
                rxSeqSet = true;
                return false;
            }
            memcpy(slotBuf, b, n);
            if (reorder_[cobsSeq].in_use) {
                free(reorder_[cobsSeq].buf);
                lostMsgs++;
            }
            reorder_[cobsSeq].buf = slotBuf;
            reorder_[cobsSeq].len = (uint16_t)n;
            reorder_[cobsSeq].heldAtMs = hw.nowMs();
            reorder_[cobsSeq].in_use = true;
        } else {
            // Zero-len (keepalive) out of
            // order: don't reserve a reorder
            // slot. A null-buf in_use slot
            // would cause pushAppBuf(null,0)
            // and a spurious ACK advancing
            // rxSeq past the real gap.
            lostMsgs++;
        }
        sendNakFrame_unlocked(exp);
        return false;
    }
    rxSeq = cobsSeq;
    rxSeqSet = true;
    if (n == 0) {
        if (errs > 0)
            errs = 0;
        return false;
    }
    int acc = hw.pushAppBuf(b, n);
    rxBytes += acc;
    if (decideAppBuf(acc, n) == AppBufAction::HoldAck) {
        Log::log().info(TAG,
                        "seq=%u app buf full "
                        "(want %d got %d)",
                        (unsigned)cobsSeq, n, acc);
        return true;
    }
    sendAckFrame_unlocked(cobsSeq);
    reorderFlushContiguous_unlocked(hw.nowMs());
    if (errs > 0)
        errs = 0;
    return false;
}

bool Link::onAck(uint8_t ackedCobsSeq)
{
    if (state != State::OK)
        return false;
    if (!ackedPending_[ackedCobsSeq])
        return false;
    ackedPending_[ackedCobsSeq] = false;
    retxCount_[ackedCobsSeq] = 0;
    baseSeq_[ackedCobsSeq] = 0;
    if (arqAckCallback_)
        arqAckCallback_(ackedCobsSeq, arqCtx_);
    return false;
}

bool Link::onNak(uint8_t missingCobsSeq)
{
    if (state != State::OK)
        return false;
    if (!ackedPending_[missingCobsSeq])
        return false;
    sentAtMs_[missingCobsSeq] = hw.nowMs();
    pendingRetxBase_ = missingCobsSeq;
    hasPendingRetx_ = true;
    retxNeeded_ = true;
    return false;
}

bool Link::onFrameError()
{
    return err_unlocked();
}

void Link::onBreak()
{
    hw.lock();
    Log::log().info(TAG, "BREAK -> resweep");
    reset_unlocked(true);
    hw.unlock();
}

void Link::onTimer()
{
    hw.lock();
    State s = state;
    int cur = spdI;
    if (s == State::OK)
        onTimerOk_unlocked();
    else if (s == State::SWP)
        onTimerSwp_unlocked();
    else if (s == State::LCK && isMaster)
        onTimerLck_unlocked();
    else {
        hw.unlock();
        (void)cur;
        return;
    }
    hw.unlock();
    if (hasPendingRetx_ && arqRetxCallback_) {
        // true = link should reset;
        // false = retx ok or no-op.
        uint8_t base = pendingRetxBase_;
        hasPendingRetx_ = false;
        if (arqRetxCallback_(base, arqCtx_)) {
            hw.lock();
            reset_unlocked(true);
            hw.unlock();
            hw.sendBreak();
        }
    }
}

void Link::onTimerOk_unlocked()
{
    if (cfg.idleTimeoutMs <= 0)
        return;
    uint32_t now = hw.nowMs();
    if (linkPaused_)
        return;
    reorderDropExpired_unlocked(now);
    {
        uint32_t rxAge = now - lastRxMs;
        uint32_t txAge = now - lastTxMs;
        // SYNC mode: skip the asymmetric
        // idle check. The sender blocks
        // inline for each frame's ACK, so
        // "TX active, RX silent" is the
        // expected steady state between
        // messages. The sender's own
        // syncAckTimeoutMs watchdog catches
        // actual hangs.
        if (cfg.mode != AutoLinkConfig::Mode::SYNC) {
            // TX active, RX silent → peer gone.
            if (rxAge > (uint32_t)FAST_IDLE_RX_MS &&
                txAge < (uint32_t)FAST_IDLE_TX_MS) {
                Log::log().warning(TAG, "asymmetric idle -> drop");
                reset_unlocked(true);
                hw.unlock();
                hw.sendBreak();
                return;
            }
        }
        if (decideIdleWatchdog(rxAge, txAge, cfg.idleTimeoutMs) ==
            IdleAction::Drop) {
            reset_unlocked(true);
            hw.unlock();
            hw.sendBreak();
            return;
        }
    }
    if ((now - lastHeartbeatMs_) >= (uint32_t)HEARTBEAT_MS) {
        lastHeartbeatMs_ = now;
        sendFrame_unlocked(PING_CMD);
        heartbeatPingsMissed_++;
        if (heartbeatPingsMissed_ >= HEARTBEAT_MISS_LIMIT) {
            Log::log().warning(TAG, "HB: %d missed -> drop",
                               heartbeatPingsMissed_);
            reset_unlocked(true);
            hw.unlock();
            hw.sendBreak();
            return;
        }
    }
    if (decideKeepalive(now - lastTxMs, cfg.idleTimeoutMs, false) ==
        KeepaliveAction::Emit) {
        sendCobsFrame_unlocked(nullptr, 0);
        lastTxMs = now;
    }
    // ASYNC only: the ARQ retransmit
    // machinery lives in the link task's
    // timer tick. In SYNC mode the sender
    // blocks inline for the receiver's ACK
    // and the ARQ pool is never populated,
    // so any ackedPending_[] bits left over
    // from a timed-out SYNC wait are stale.
    // Running the retransmit loop here would
    // call arqRetxCallback_ for slots whose
    // pool buffer was never inserted, i.e.
    // spurious retransmits.
    if (cfg.mode != AutoLinkConfig::Mode::SYNC) {
        for (int s = 0; s < 256; s++) {
            if (!ackedPending_[s])
                continue;
            uint32_t age = now - sentAtMs_[s];
            ArqAction a =
                decideArqSlot(age, retxCount_[s], ACK_RTO_MS, MAX_RETX);
            if (a == ArqAction::Hold)
                continue;
            if (a == ArqAction::Drop) {
                Log::log().error(TAG, "seq=%u MAX_RETX -> drop", (unsigned)s);
                reset_unlocked(true);
                hw.unlock();
                hw.sendBreak();
                return;
            }
            retxCount_[s]++;
            sentAtMs_[s] = now;
            if (arqRetxCallback_) {
                pendingRetxBase_ = s;
                hasPendingRetx_ = true;
            }
            retxNeeded_ = true;
            break;
        }
    }
    hw.startTimer(okTickMs());
}

int Link::pendingAcks() const
{
    int n = 0;
    for (int i = 0; i < 256; i++)
        if (ackedPending_[i])
            n++;
    return n;
}
bool Link::isAcked(uint8_t cobsSeq) const
{
    return !ackedPending_[cobsSeq];
}

int Link::popRetransmitSlot()
{
    uint32_t now = hw.nowMs();
    for (int s = 0; s < 256; s++) {
        if (!ackedPending_[s])
            continue;
        if (now - sentAtMs_[s] < ACK_RTO_MS)
            continue;
        uint8_t seq = (uint8_t)s;
        ackedPending_[seq] = false;
        retxCount_[seq]++;
        sentAtMs_[seq] = now;
        return seq;
    }
    return -1;
}

void Link::onTimerSwp_unlocked()
{
    if (isMaster) {
        if (sweepPhase_ == SweepPhase::PHASE1) {
            sendFrame_unlocked(PING_CMD);
            pingSample++;
            hw.startTimer(phase1ArmMs());
            return;
        }
        if (sweepPhase_ == SweepPhase::PHASE2) {
            spdI++;
            if (spdI >= (int)cfg.allowedBaudsCount) {
                sweepPhase_ = SweepPhase::NONE;
                lockOk_unlocked(cfg.allowedBaudsCount - 1, "p2-fallback");
                return;
            }
            hw.setSpd(cfg.allowedBauds[spdI]);
            sendFrame_unlocked(PING_CMD);
            hw.startTimer(dwells_.phase2[spdI]);
            return;
        }
        if (sweepPhase_ == SweepPhase::PHASE3) {
            int next = phase3Baud_ + 1;
            phase3Baud_ = -1;
            phase3Acks_ = 0;
            if (next >= (int)cfg.allowedBaudsCount) {
                int lb = cfg.allowedBaudsCount - 1;
                sweepPhase_ = SweepPhase::NONE;
                spdI = lb;
                hw.setSpd(cfg.allowedBauds[lb]);
                lockOk_unlocked(lb, "p3-fallback");
                return;
            }
            sweepPhase_ = SweepPhase::PHASE2;
            spdI = next;
            hw.setSpd(cfg.allowedBauds[spdI]);
            sendFrame_unlocked(PING_CMD);
            hw.startTimer(dwells_.phase2[spdI]);
            return;
        }
        enterPhase1_unlocked();
        return;
    }
    if (emptySweeps == 0 || emptySweeps % 5 == 0) {
        Log::log().info(TAG,
                        "pong SWP baud[%d]=%lu "
                        "phase=%d",
                        spdI, (unsigned long)cfg.allowedBauds[spdI],
                        (int)sweepPhase_);
    }
    emptySweeps++;
    if (emptySweeps > 10) {
        if (!wasEverOk_) {
            // First boot, no PING ever:
            // likely wiring fault.
            Log::log().error(TAG,
                             "WIRING? %d ticks no PING:"
                             " need TX->RX crossover,"
                             " shared GND",
                             emptySweeps);
        }
        emptySweeps = 5;
    }
    if (sweepPhase_ == SweepPhase::PHASE1) {
        hw.startTimer(phase1ArmMs());
        return;
    }
    if (sweepPhase_ == SweepPhase::PHASE2) {
        int dwell = dwells_.phase2Slave[spdI];
        spdI--;
        if (spdI < 0) {
            enterPhase1_unlocked();
            return;
        }
        hw.setSpd(cfg.allowedBauds[spdI]);
        hw.startTimer(dwell);
        return;
    }
    hw.startTimer(dwells_.phase2[spdI]);
}

void Link::onTimerLck_unlocked()
{
    int max = (int)cfg.allowedBaudsCount * 2;
    lckRetries++;
    if (decideLckTick(lckRetries, max) == LckAction::SendReq) {
        sendFrame_unlocked(REQ_CMD);
        hw.startTimer(cfg.delayMs);
    } else {
        reset_unlocked(true);
        hw.unlock();
        hw.sendBreak();
    }
}

bool Link::test_sendMsgBegin(const uint8_t *b, int len)
{
    if (cfg.mode != AutoLinkConfig::Mode::SYNC)
        return false;
    if (len < 0 || (size_t)len > cfg.maxMsg)
        return false;
    if (len == 0)
        return true;
    uint16_t c = UtilCrc::crc16(b, len);
    uint8_t hdr[MSG_HDR] = { (uint8_t)(len),       (uint8_t)(len >> 8),
                             (uint8_t)(len >> 16), (uint8_t)(len >> 24),
                             (uint8_t)(c),         (uint8_t)(c >> 8) };
    hw.lock();
    if (state != State::OK) {
        hw.unlock();
        return false;
    }
    uint8_t seq = txSeq;
    if (len + MSG_HDR <= MAX_CHUNK) {
        uint8_t merged[MAX_CHUNK];
        memcpy(merged, hdr, MSG_HDR);
        memcpy(merged + MSG_HDR, b, len);
        sendCobsFrame_unlocked(merged, MSG_HDR + len);
        txBytes += len;
        lastTxMs = hw.nowMs();
    } else {
        sendCobsFrame_unlocked(hdr, MSG_HDR);
        txBytes += 0;
        lastTxMs = hw.nowMs();
        int offset = 0;
        while (offset < len) {
            int chunk = std::min(len - offset, MAX_CHUNK);
            seq = txSeq;
            sendCobsFrame_unlocked(b + offset, chunk);
            txBytes += chunk;
            lastTxMs = hw.nowMs();
            offset += chunk;
        }
    }
    ackedPending_[seq] = true;
    retxCount_[seq] = 0;
    hw.unlock();
    return true;
}

bool Link::test_sendMsgStillWaiting()
{
    if (cfg.mode != AutoLinkConfig::Mode::SYNC)
        return false;
    hw.lock();
    bool any = false;
    for (int i = 0; i < 256; i++) {
        if (ackedPending_[i]) {
            any = true;
            break;
        }
    }
    hw.unlock();
    return any;
}

} // namespace autolink
