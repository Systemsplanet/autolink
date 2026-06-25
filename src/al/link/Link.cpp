// Wire-protocol coordinator. State lives in
// LinkArq / LinkReorder / LinkSweep; Link
// composes them and owns I/O.
#include "al/link/Link.h"
#include "al/link/arq/LinkArq.h"
#include "al/link/sweep/LinkDecision.h"
#include "al/link/LinkReorder.h"
#include "al/link/sweep/LinkSweep.h"
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

namespace autolink {
static_assert(MAX_CHUNK + 6 <= 256, "MAX_CHUNK too large");

static constexpr int HEARTBEAT_MS = 100;
static constexpr int HEARTBEAT_MISS_LIMIT = 3;
// Asymmetric idle: TX active, RX silent
// → peer gone, drop fast.
static constexpr int FAST_IDLE_RX_MS = 300;
static constexpr int FAST_IDLE_TX_MS = 1000;

int Link::okTickMs() const {
    int k = cfg.idleTimeoutMs / 3;
    if (k < 50)
        k = 50;
    return k < cfg.syncAckTimeoutMs ? cfg.syncAckTimeoutMs : k;
}

int Link::phase1ArmMs() { return sweep_.phase1ArmMs(*this); }

const char *StateToStr(State s) {
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

Link::Link(IHal &h, IArqCache &cache, bool isMasterNode,
           const AutoLinkConfig &config)
    : hw(h), arqCache_(cache), isMaster(isMasterNode), cfg(config),
      state(State::OK), errs(0), spdI(0), pingSample(0), emptySweeps(0),
      baudSweep((int)config.allowedBaudsCount), rxIdx(0), frameRx(*this),
      rxMsgLen(-1), rxMsgCrc(0), lckRetries(0), lastRxMs(0), lastTxMs(0),
      txBytes(0), rxBytes(0), discCount(0), frameErrs(0) {
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

Link::~Link() = default;

void Link::resetSeq_unlocked() {
    txSeq = 0;
    rxSeqSet = false;
    rxSeq = 0;
}

uint8_t Link::reorderExpectedSeq() const {
    return (uint8_t)((rxSeq == (uint8_t)COBS_SEQ_MAX) ? 0 : rxSeq + 1);
}

void Link::reorderAdvanceRxSeq(uint8_t seq) {
    rxSeq = seq;
    rxSeqSet = true;
}

void Link::begin() {
    // Pure init: dwell computation, internal state.
    // Wire-side effects (sendBreak, sweep entry, baud
    // arm) happen via kickoff(). When linkPaused_ is
    // false (the default — host tests, Pong, etc.),
    // kickoff fires here so the legacy "begin() = go"
    // contract holds; when the caller has set
    // linkPaused_=true before begin() (Ping's startup-
    // order case), the kickoff is deferred to the
    // explicit kickoff() call that fires when the user
    // pushes Start.
    hw.lock();
    sweep_.computeDwells(*this);
    hw.unlock();
    kickedOff_ = false;
    if (linkPaused_) {
        Log::log().info(TAG,
                        "begin: link initialised; kickoff deferred "
                        "(linkPaused=true)");
        return;
    }
    kickoff();
}

void Link::kickoff() {
    if (kickedOff_) {
        Log::log().debug(TAG, "kickoff: already running; no-op");
        return;
    }
    if (linkPaused_) {
        Log::log().warning(TAG,
                           "kickoff: linkPaused_=true; refusing to fire "
                           "wire-side start. Call setLinkPaused(false) "
                           "first.");
        return;
    }
    kickedOff_ = true;
    if (isMaster) {
        hw.lock();
        reset_unlocked(false);
        sweep_.enterPhase1(*this);
        hw.unlock();
        hw.sendBreak();
        Log::log().info(TAG, "kickoff: master sent break; entering P1");
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
        sweep_.setPhase(SweepPhase::PHASE1);
        hw.unlock();
        hw.clearAppBuf();
        hw.setSpd(cfg.allowedBauds[spdI]);
        Log::log().info(TAG, "SWP Pong P1 baud[%d]=%lu", spdI,
                        (unsigned long)cfg.allowedBauds[spdI]);
        hw.startTimer(cfg.pingSamplesPerBaud * cfg.delayMs);
        Log::log().info(TAG, "kickoff: slave armed P1 listener");
    }
}

void Link::changeState_unlocked(State newState) {
    if (state != newState) {
        Log::log().debug(TAG, "%s -> %s", StateToStr(state),
                         StateToStr(newState));
        state = newState;
    }
}

int Link::bestSpd_unlocked() const {
    int best = baudSweep.pickBest();
    if (best < 0)
        return 0;
    for (int j = 0; j < best; j++)
        if (baudSweep.scoreAt(j) > 0)
            return j;
    return best;
}

void Link::sendPongAck_unlocked() { sendFrame_unlocked(PONG_CMD); }

void Link::sendFrame_unlocked(uint8_t payload) {
    uint8_t frame[CTRL_FRAME_SIZE];
    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[CTRL_FRAME_SEQ_IDX] = txSeq;
    frame[CTRL_FRAME_PAYLOAD_IDX] = payload;
    frame[CTRL_FRAME_CRC_IDX] = UtilCrc::crc8(frame, CTRL_FRAME_SIZE - 1);
    if (hw.tx(frame, CTRL_FRAME_SIZE) != CTRL_FRAME_SIZE)
        Log::log().error(TAG, "sendFrame truncated");
}

void Link::buildAndTxCobsFrame_unlocked(uint8_t seq, const uint8_t *b, int n) {
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

void Link::sendCobsFrame_unlocked(const uint8_t *b, int n) {
    uint8_t seq = txSeq;
    buildAndTxCobsFrame_unlocked(seq, b, n);
    // Skip 0xFE/0xFF: wire discriminators.
    txSeq = (txSeq == COBS_SEQ_MAX) ? 0 : (uint8_t)(txSeq + 1);
}

uint8_t Link::sendCobsFrameAcked_unlocked(const uint8_t *b, int n,
                                          uint8_t baseSeq) {
    uint8_t seq = txSeq;
    sendCobsFrame_unlocked(b, n);
    arq_.onSent(seq, baseSeq, hw.nowMs());
    if (n > 0)
        arqCache_.insert(seq, b, n);
    return seq;
}

void Link::resendCobsFrame_unlocked(uint8_t seq, const uint8_t *b, int n) {
    buildAndTxCobsFrame_unlocked(seq, b, n);
}

void Link::sendCtrlCobsFrame_unlocked(uint8_t type, uint8_t seq) {
    uint8_t u[3] = { type, seq, 0 };
    u[2] = UtilCrc::crc8(u, 2);
    uint8_t frame[8];
    frame[0] = 0x00;
    size_t el = UtilCobs::encode(u, 3, frame + 1);
    frame[1 + el] = 0x00;
    hw.tx(frame, (int)(el + 2));
}

void Link::sendAckFrame_unlocked(uint8_t ackedCobsSeq) {
    sendCtrlCobsFrame_unlocked(ACK_TYPE, ackedCobsSeq);
}

void Link::sendNakFrame_unlocked(uint8_t missingCobsSeq) {
    sendCtrlCobsFrame_unlocked(NAK_TYPE, missingCobsSeq);
}

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

bool Link::buildAndSendMsg_unlocked(const uint8_t *b, int len,
                                    uint8_t *outLastSeq) {
    uint16_t c = UtilCrc::crc16(b, len);
    uint8_t hdr[MSG_HDR] = { (uint8_t)(len),       (uint8_t)(len >> 8),
                             (uint8_t)(len >> 16), (uint8_t)(len >> 24),
                             (uint8_t)(c),         (uint8_t)(c >> 8) };
    uint8_t lastSeq = 0;
    if (len + MSG_HDR <= MAX_CHUNK) {
        // Short msg: coalesce hdr+data
        // into one wire frame.
        uint8_t merged[MAX_CHUNK];
        memcpy(merged, hdr, MSG_HDR);
        memcpy(merged + MSG_HDR, b, len);
        lastSeq = txSeq;
        sendCobsFrame_unlocked(merged, MSG_HDR + len);
        txBytes += len;
        lastTxMs = hw.nowMs();
    } else {
        lastSeq = txSeq;
        sendCobsFrame_unlocked(hdr, MSG_HDR);
        txBytes += 0;
        lastTxMs = hw.nowMs();
        int offset = 0;
        while (offset < len) {
            int chunk = std::min(len - offset, MAX_CHUNK);
            lastSeq = txSeq;
            sendCobsFrame_unlocked(b + offset, chunk);
            txBytes += chunk;
            lastTxMs = hw.nowMs();
            offset += chunk;
        }
    }
    if (outLastSeq)
        *outLastSeq = lastSeq;
    return true;
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

int Link::findMsgHeaderResync_unlocked(int max_scan) {
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

void Link::getStats(Stats &s) const {
    hw.lock();
    s.tx = txBytes;
    s.rx = rxBytes;
    s.discCount = discCount;
    s.frameErrs = frameErrs;
    hw.unlock();
}
void Link::resetStats() {
    hw.lock();
    txBytes = rxBytes = 0;
    hw.unlock();
}
void Link::resetErrors() {
    hw.lock();
    discCount = frameErrs = 0;
    errWindowCount_ = 0;
    errWindowStartMs_ = hw.nowMs();
    hw.unlock();
}
void Link::resetDiag() {
    hw.lock();
    gaps = stale = lostMsgs = 0;
    hw.unlock();
}

State Link::getState() const {
    hw.lock();
    State s = state;
    hw.unlock();
    return s;
}
int Link::getErrCount() const {
    hw.lock();
    int e = errs;
    hw.unlock();
    return e;
}
int Link::getCurrentSpdIndex() const {
    hw.lock();
    int i = spdI;
    hw.unlock();
    return i;
}
uint32_t Link::getCurrentBaud() const {
    hw.lock();
    uint32_t b = (spdI >= 0 && spdI < (int)cfg.allowedBaudsCount)
        ? cfg.allowedBauds[spdI]
        : 0;
    hw.unlock();
    return b;
}
void Link::getDiag(Diag &d) const {
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

void Link::lockOk_unlocked(int idx, const char *tag) {
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

void Link::onRx(const uint8_t *data, int len) {
    hw.lock();
    int i = 0;
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
                        if (processCtrlFrame_unlocked(cur))
                            needBreak = true;
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
                if (processCtrlFrame_unlocked(cur)) {
                    needBreak = true;
                    break;
                }
            }
        }
    }
    hw.unlock();
    if (needBreak)
        hw.sendBreak();
}

bool Link::processCtrlFrame_unlocked(State cur) {
    if (UtilCrc::crc8(rxBuf, CTRL_FRAME_SIZE - 1) !=
        rxBuf[CTRL_FRAME_CRC_IDX]) {
        return err_unlocked();
    }
    uint8_t cs = rxBuf[CTRL_FRAME_SEQ_IDX];
    uint8_t pl = rxBuf[CTRL_FRAME_PAYLOAD_IDX];
    if (cur == State::OK) {
        // Heartbeat / link-poll. PING →
        // pong immediately so the peer's
        // heartbeat-miss counter stays
        // reset; PONG → clear our own
        // miss counter.
        if (pl == PING_CMD)
            sendPongAck_unlocked();
        else if (pl == PONG_CMD)
            heartbeatPingsMissed_ = 0;
        return false;
    }
    return ctrlFrameReady_unlocked(cs, pl, cur);
}

bool Link::ctrlFrameReady_unlocked(uint8_t cs, uint8_t pl, State cur) {
    if (cur == State::SWP)
        return handleSwp_unlocked(cs, pl);
    if (cur == State::LCK)
        return handleLck_unlocked(cs, pl);
    return false;
}

bool Link::handleSwp_unlocked(uint8_t cobsSeq, uint8_t payload) {
    (void)cobsSeq;
    if (!isMaster && payload == REQ_CMD) {
        int best = bestSpd_unlocked();
        sendFrame_unlocked((uint8_t)best);
        lockOk_unlocked(best, "REQ");
        return false;
    }
    if (payload >= LOCK_CMD && payload < LOCK_CMD + cfg.allowedBaudsCount) {
        int lb = (int)(payload - LOCK_CMD);
        hw.setSpd(cfg.allowedBauds[lb]);
        spdI = lb;
        sweep_.reset();
        lockOk_unlocked(lb,
                        sweep_.phase() == SweepPhase::PHASE3
                            ? (isMaster ? "p3-lock" : "p3-pong")
                            : "lock");
        return false;
    }
    if (payload == PONG_CMD && isMaster) {
        if (sweep_.phase() == SweepPhase::PHASE1) {
            // First PONG at any baud means the link is up; sweep
            // all bauds (P2) and confirm the best with 2-of-3 (P3)
            // before locking. Locking on the first contact would
            // commit to whatever baud the PONG happened to arrive
            // at — which is not the link's best baud in general.
            sweep_.enterPhase2(*this);
            return false;
        }
        if (sweep_.phase() == SweepPhase::PHASE2) {
            sweep_.enterPhase3(*this, spdI);
            return false;
        }
        if (sweep_.phase() == SweepPhase::PHASE3) {
            int acks = sweep_.phase3Acks() + 1;
            int baud = sweep_.phase3Baud();
            // Mutate in place via reset+reset is awkward;
            // bump counter directly. Phase state is owned by
            // sweep_; ack counter increments here.
            // (No public API for incPhase3Acks yet — but
            //  LinkSweep's enterPhase3 resets it, and the
            //  only path that increments is this one. Add
            //  an accessor.)
            // We use a tiny helper: bump ack count.
            // To keep LinkSweep clean, expose incAcks().
            // (See LinkSweep::incPhase3Acks below.)
            sweep_.incPhase3Acks();
            if (acks >= 2) {
                sweep_.reset();
                hw.setSpd(cfg.allowedBauds[baud]);
                spdI = baud;
                sendFrame_unlocked(LOCK_CMD + (uint8_t)baud);
                lockOk_unlocked(baud, "phase3");
                return false;
            }
            sendFrame_unlocked(PING_CMD);
            int rt =
                (int)(2.0 * (5.0 * 10.0 / cfg.allowedBauds[baud] * 1000.0) +
                      0.5);
            if (rt < 50)
                rt = 50;
            int t3 = rt * (2 - acks + 1) + 100;
            if (t3 < 200)
                t3 = 200;
            hw.startTimer(t3);
            heartbeatPingsMissed_ = 0;
            return false;
        }
    }
    if (payload == PING_CMD && !isMaster) {
        baudSweep.score(spdI);
        if (sweep_.phase() == SweepPhase::PHASE1) {
            sendPongAck_unlocked();
        } else if (sweep_.phase() == SweepPhase::PHASE2) {
            sweep_.setPhase(SweepPhase::PHASE3);
            int b = spdI;
            // Manually advance via reset+enterPhase3.
            sweep_.reset();
            sweep_.enterPhase3(*this, b);
        } else if (sweep_.phase() == SweepPhase::PHASE3) {
            sweep_.incPhase3Acks();
            if (sweep_.phase3Acks() >= 2) {
                int lb = sweep_.phase3Baud();
                sweep_.reset();
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

bool Link::handleLck_unlocked(uint8_t cobsSeq, uint8_t payload) {
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

void Link::reset_unlocked(bool count) {
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
    emptySweeps = 0;
    errWindowStartMs_ = hw.nowMs();
    errWindowCount_ = 0;
    lastRxMs = hw.nowMs();
    arq_.clearAll();
    hasPendingRetx_ = false;
    pendingRetxBase_ = NO_BASE;
    reorder_.clearAll();
    resetSeq_unlocked();
    hw.clearAppBuf();
    sweep_.enterPhase1(*this);
    arqCache_.clearAll();
}

bool Link::onPayload(uint8_t cobsSeq, const uint8_t *b, int n) {
    if (state != State::OK)
        return true;
    // Drop expired reorder slots before
    // classifying this frame. lostMsgs is
    // incremented here, on the per-slot
    // expiry, so a retransmit that lands
    // in time doesn't count as lost.
    int dropped = reorder_.dropExpired(hw.nowMs(), cfg.reorderHoldMs);
    if (dropped > 0)
        lostMsgs += (uint64_t)dropped;
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
        uint8_t exp = reorderExpectedSeq();
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
            bool newlyHeld = reorder_.hold(cobsSeq, b, n, hw.nowMs());
            if (!newlyHeld) {
                // hold() returns false on OOM
                // (malloc failure) too. Drop
                // the slot and bump lostMsgs.
                lostMsgs++;
                rxSeq = cobsSeq;
                rxSeqSet = true;
                return false;
            }
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
    reorder_.flushContiguous(*this, hw.nowMs());
    if (errs > 0)
        errs = 0;
    return false;
}

bool Link::onAck(uint8_t ackedCobsSeq) {
    if (state != State::OK)
        return false;
    if (!arq_.isPending(ackedCobsSeq))
        return false;
    arq_.onAcked(ackedCobsSeq);
    arqCache_.freeBySeq(ackedCobsSeq);
    return false;
}

bool Link::onNak(uint8_t missingCobsSeq) {
    if (state != State::OK)
        return false;
    if (!arq_.isPending(missingCobsSeq))
        return false;
    arq_.onNaked(missingCobsSeq, hw.nowMs());
    pendingRetxBase_ = missingCobsSeq;
    hasPendingRetx_ = true;
    return false;
}

bool Link::onFrameError() { return err_unlocked(); }

void Link::onBreak() {
    hw.lock();
    Log::log().info(TAG, "BREAK -> resweep");
    reset_unlocked(true);
    hw.unlock();
}

void Link::onTimer() {
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
    if (hasPendingRetx_) {
        // Cache holds the payload; link drives
        // the resend. A cache miss is a no-op
        // (the ARQ bit will time out on its own).
        uint8_t base = pendingRetxBase_;
        hasPendingRetx_ = false;
        const uint8_t *buf = nullptr;
        int len = 0;
        if (!arqCache_.slotInUse(base)) {
            Log::log().info(TAG,
                            "ARQ retx cobsSeq=%u cache miss (chunk already "
                            "delivered); pending bit left to time out",
                            (unsigned)base);
        } else if (arqCache_.peekForRetx(base, &buf, &len)) {
            Log::log().warning(TAG, "ARQ retx cobsSeq=%u (%d bytes) — verbatim",
                               (unsigned)base, len);
            resendCobsFrame_unlocked(base, buf, len);
        } else {
            Log::log().info(TAG,
                            "ARQ retx cobsSeq=%u (keepalive, no pool buf) — "
                            "verbatim 0 bytes",
                            (unsigned)base);
            resendCobsFrame_unlocked(base, nullptr, 0);
        }
    }
}

void Link::onTimerOk_unlocked() {
    if (cfg.idleTimeoutMs <= 0)
        return;
    uint32_t now = hw.nowMs();
    if (linkPaused_)
        return;
    int dropped = reorder_.dropExpired(now, cfg.reorderHoldMs);
    if (dropped > 0)
        lostMsgs += (uint64_t)dropped;
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
    // call arqCache_.peekForRetx() for slots whose
    // pool buffer was never inserted, i.e.
    // spurious retransmits.
    if (cfg.mode != AutoLinkConfig::Mode::SYNC) {
        for (int s = 0; s < 256; s++) {
            if (!arq_.isPending(s))
                continue;
            LinkArq::Action a = arq_.decideSlot(
                (uint8_t)s, now, (uint32_t)cfg.syncAckTimeoutMs, cfg.maxRetx);
            if (a == LinkArq::Action::Hold)
                continue;
            if (a == LinkArq::Action::Drop) {
                Log::log().error(TAG, "seq=%u maxRetx -> drop", (unsigned)s);
                reset_unlocked(true);
                hw.unlock();
                hw.sendBreak();
                return;
            }
            arq_.applyRetx((uint8_t)s, now);
            pendingRetxBase_ = (uint8_t)s;
            hasPendingRetx_ = true;
            break;
        }
    }
    hw.startTimer(okTickMs());
}

int Link::pendingAcks() const {
    hw.lock();
    int n = arq_.pendingCount();
    hw.unlock();
    return n;
}
bool Link::isAcked(uint8_t cobsSeq) const { return arq_.isAcked(cobsSeq); }

void Link::onTimerSwp_unlocked() {
    if (isMaster) {
        if (sweep_.phase() == SweepPhase::PHASE1) {
            sendFrame_unlocked(PING_CMD);
            pingSample++;
            hw.startTimer(phase1ArmMs());
            return;
        }
        if (sweep_.phase() == SweepPhase::PHASE2) {
            spdI++;
            if (spdI >= (int)cfg.allowedBaudsCount) {
                sweep_.reset();
                hw.setSpd(cfg.allowedBauds[cfg.allowedBaudsCount - 1]);
                spdI = cfg.allowedBaudsCount - 1;
                sendFrame_unlocked(LOCK_CMD +
                                   (uint8_t)(cfg.allowedBaudsCount - 1));
                lockOk_unlocked(cfg.allowedBaudsCount - 1, "p2-fallback");
                return;
            }
            hw.setSpd(cfg.allowedBauds[spdI]);
            sendFrame_unlocked(PING_CMD);
            hw.startTimer(sweep_.dwells().phase2[spdI]);
            return;
        }
        if (sweep_.phase() == SweepPhase::PHASE3) {
            int next = sweep_.phase3Baud() + 1;
            sweep_.reset();
            if (next >= (int)cfg.allowedBaudsCount) {
                int lb = cfg.allowedBaudsCount - 1;
                spdI = lb;
                hw.setSpd(cfg.allowedBauds[lb]);
                lockOk_unlocked(lb, "p3-fallback");
                return;
            }
            sweep_.setPhase(SweepPhase::PHASE2);
            spdI = next;
            hw.setSpd(cfg.allowedBauds[spdI]);
            sendFrame_unlocked(PING_CMD);
            hw.startTimer(sweep_.dwells().phase2[spdI]);
            return;
        }
        sweep_.enterPhase1(*this);
        return;
    }
    if (emptySweeps == 0 || emptySweeps % 5 == 0) {
        Log::log().info(TAG,
                        "pong SWP baud[%d]=%lu "
                        "phase=%d",
                        spdI, (unsigned long)cfg.allowedBauds[spdI],
                        (int)sweep_.phase());
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
    if (sweep_.phase() == SweepPhase::PHASE1) {
        hw.startTimer(phase1ArmMs());
        return;
    }
    if (sweep_.phase() == SweepPhase::PHASE2) {
        int dwell = sweep_.dwells().phase2Slave[spdI];
        spdI--;
        if (spdI < 0) {
            sweep_.enterPhase1(*this);
            return;
        }
        hw.setSpd(cfg.allowedBauds[spdI]);
        hw.startTimer(dwell);
        return;
    }
    hw.startTimer(sweep_.dwells().phase2[spdI]);
}

void Link::onTimerLck_unlocked() {
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