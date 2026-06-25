// Wire-protocol implementation. See Link.h for the
// contract. Pure decisions (state transitions, gap
// classification) live in LinkDecision.h so they're
// table-testable without I/O.
#include "al/link/Link.h"
#include "al/link/LinkDecision.h"
#include "al/util/Log.h"
#include <cstdio>

#ifdef ARDUINO
#    if defined(ESP_PLATFORM) || defined(ESP32) || \
        defined(ARDUINO_ARCH_ESP32)
#        include <freertos/FreeRTOS.h>
#    endif
#endif
#include "al/util/UtilCrc.h"
#include "al/util/UtilCobs.h"
#include <algorithm>
#include <string.h>

static constexpr const char *ALINK_TAG = "AutoLink";

namespace autolink
{
static_assert(
    MAX_CHUNK + 6 <= 256,
    "MAX_CHUNK too large for 256-byte frame buffers");

static constexpr int PHASE1_MAX_TRIES = 6;
static constexpr int PHASE3_ACKS_NEEDED = 2;
static constexpr int HEARTBEAT_INTERVAL_MS = 100;
static constexpr int HEARTBEAT_MISS_LIMIT = 3;
static constexpr int FAST_IDLE_RX_MS = 300;
static constexpr int FAST_IDLE_TX_MS = 1000;
// Asymmetric fast detection: local TX within
// FAST_IDLE_TX_MS but no RX within FAST_IDLE_RX_MS
// means the peer is silent — drop.

int Link::okTickMs() const
{
    int keep = cfg.idleTimeoutMs / 3;
    if (keep < 50)
        keep = 50;
    return keep < (int)ACK_RTO_MS ? (int)ACK_RTO_MS
                                  : keep;
}

int Link::phase1ArmMs()
{
#ifdef AUTOLINK_HOST_TEST
    return dwells_.phase1;
#else
    return jitterPhase1Dwell(
        dwells_.phase1,
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

Link::Link(IHal &h, bool isMasterNode,
           const AutoLinkConfig &config)
    : hw(h), isMaster(isMasterNode), cfg(config),
      state(State::OK), errs(0), spdI(0),
      pingSample(0), emptySweeps(0),
      baudSweep((int)config.allowedBaudsCount),
      rxIdx(0), frameRx(*this), rxMsgLen(-1),
      rxMsgCrc(0), lckRetries(0), lastRxMs(0),
      lastTxMs(0), txBytes(0), rxBytes(0),
      discCount(0), frameErrs(0)
{
    UtilBaudSweep::Config sc;
    sc.pingSamplesPerBaud = config.pingSamplesPerBaud;
    sc.minAcceptRate = config.minAcceptRate;
    sc.expectedSamples = -1;
    baudSweep.configure(sc);
    hw.bind(this);
    Log::log().info(
        ALINK_TAG,
        "Initialized as %s. cobsSeq tracking: ON",
        isMaster ? "Ping" : "Pong");
    if (cfg.maxMsg > cfg.streamBufferSize) {
        Log::log().error(
            ALINK_TAG,
            "maxMsg (%u) > streamBufferSize (%u): "
            "large messages cannot be reassembled",
            (unsigned)cfg.maxMsg,
            (unsigned)cfg.streamBufferSize);
    }
}

Link::~Link()
{
    for (int i = 0; i < 256; i++) {
        if (reorder_[i].buf) {
            free(reorder_[i].buf);
            reorder_[i].buf = nullptr;
        }
        reorder_[i].in_use = false;
        reorder_[i].len = 0;
    }
}

void Link::reorderClear_unlocked()
{
    for (int i = 0; i < 256; i++) {
        if (reorder_[i].buf) {
            free(reorder_[i].buf);
            reorder_[i].buf = nullptr;
        }
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
        if (age < (uint32_t)cfg.arqReorderHoldMs)
            continue;
        free(reorder_[i].buf);
        reorder_[i].buf = nullptr;
        reorder_[i].in_use = false;
        reorder_[i].len = 0;
        lostMsgs++;
        Log::log().warning(
            ALINK_TAG,
            "reorder: cobsSeq=%u expired (held %u ms, "
            "cap %d) -> dropped, lostMsgs++",
            (unsigned)i, (unsigned)age,
            cfg.arqReorderHoldMs);
    }
}

int Link::reorderFlushContiguous_unlocked(uint32_t)
{
    int delivered = 0;


    bool progress = true;
    while (progress) {
        progress = false;
        uint8_t expected =
            (uint8_t)((rxSeq ==
                       (uint8_t)(COBS_SEQ_MAX))
                          ? 0
                          : (rxSeq + 1));
        if (!reorder_[expected].in_use)
            break;
        ReorderSlot &s = reorder_[expected];

        int acc = hw.pushAppBuf(s.buf, s.len);
        if (acc < s.len) {
            Log::log().info(
                ALINK_TAG,
                "reorder flush cobsSeq=%u app buf "
                "full (wanted %d accepted %d) — will "
                "retry next tick",
                (unsigned)expected, (int)s.len, acc);
            break;
        }
        sendAckFrame_unlocked(expected);
        rxSeq = expected;
        rxSeqSet = true;
        rxBytes += s.len;
        Log::log().debug(
            ALINK_TAG,
            "reorder flush cobsSeq=%u  %d bytes "
            "DELIVERED (post-gap fill, in-order)",
            (unsigned)expected, (int)s.len);
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

    Log::log().debug(
        ALINK_TAG,
        "cobsSeq reset (sender=%u, lastRx cleared)",
        (unsigned)txSeq);
}

void Link::computeDwells_unlocked()
{
    int N = cfg.allowedBaudsCount;
    if (N > (int)(sizeof(dwells_.phase2) /
                  sizeof(dwells_.phase2[0]))) {
        N = sizeof(dwells_.phase2) /
            sizeof(dwells_.phase2[0]);
    }
    for (int i = 0; i < N; i++) {
        double rt = 2.0 *
                (5.0 * 10.0 / cfg.allowedBauds[i] *
                 1000.0) +
            0.5;
        int d = (int)(rt * 1.5) + 1;
        if (d < 5)
            d = 5;
        dwells_.phase2[i] = d;
        dwells_.phase2Slave[i] = d;
    }
    double rt0 = 2.0 *
            (5.0 * 10.0 / cfg.allowedBauds[0] *
             1000.0) +
        0.5;
    dwells_.phase3 = (int)(3.0 * rt0 * 1.5) + 1;
    dwells_.phase1 = cfg.delayMs;
    int total = 0;
    for (int i = 0; i < N; i++)
        total += dwells_.phase2[i];
    dwells_.phase2Total = total * 5 + 200;


    if (dwells_.phase2[0] < 5) {
        for (int i = 0; i < N; i++) {
            if (dwells_.phase2[i] < 5)
                dwells_.phase2[i] = 5;
            if (dwells_.phase2Slave[i] < 5)
                dwells_.phase2Slave[i] = 5;
        }
    }
}

void Link::begin()
{
    if (cfg.allowedBaudsCount != 0) {
        Log::log().info(
            ALINK_TAG,
            "%s: %d bauds %lu..%lu, %d samples/baud",
            isMaster ? "Ping" : "Pong",
            (int)cfg.allowedBaudsCount,
            (unsigned long)cfg.allowedBauds[0],
            (unsigned long)cfg
                .allowedBauds[cfg.allowedBaudsCount -
                              1],
            cfg.pingSamplesPerBaud);
    }
    hw.lock();
    computeDwells_unlocked();
    if (cfg.allowedBaudsCount > 0) {
        Log::log().info(
            ALINK_TAG,
            "P1 dwell=%dms/retry (no exit on timeout)",
            dwells_.phase1);
        for (int i = 0; i < cfg.allowedBaudsCount;
             i++) {
            Log::log().info(
                ALINK_TAG,
                "P2 dwell master baud[%d]=%lu: %dms; "
                "slave: %dms",
                i, (unsigned long)cfg.allowedBauds[i],
                dwells_.phase2[i],
                dwells_.phase2Slave[i]);
        }
        Log::log().info(
            ALINK_TAG, "P3 max dwell=%dms (%d-of-%d)",
            dwells_.phase3, PHASE3_ACKS_NEEDED,
            PHASE3_ACKS_NEEDED + 1);
    }
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
        Log::log().info(
            ALINK_TAG,
            "SWP Pong listening at baud[%d]=%lu "
            "(Phase 1)",
            spdI,
            (unsigned long)cfg.allowedBauds[spdI]);
        hw.startTimer(cfg.pingSamplesPerBaud *
                      cfg.delayMs);
    }
}

void Link::changeState_unlocked(State newState)
{
    if (state != newState) {
        Log::log().debug(
            ALINK_TAG, "State Transition: %s -> %s",
            StateToStr(state), StateToStr(newState));
        state = newState;
    }
}

int Link::bestSpd_unlocked() const
{
    int best = baudSweep.pickBest();
    if (best < 0)
        return 0;
    for (int j = 0; j < best; j++) {
        if (baudSweep.scoreAt(j) > 0)
            return j;
    }
    return best;
}

void Link::sendPongAck_unlocked()
{
    sendFrame_unlocked(PONG_CMD);
}

void Link::enterPhase1_unlocked()
{
    // Per directive: never leave Phase 1 until
    // connected. Master sits at slowest baud; pong
    // mirrors the same contract.
    sweepPhase_ = SweepPhase::PHASE1;
    spdI = cfg.allowedBaudsCount - 1;
    pingSample = 0;
    hw.setSpd(cfg.allowedBauds[spdI]);
    Log::log().info(
        ALINK_TAG,
        "=== PHASE 1: connect at slowest baud[%d]=%lu "
        "===",
        spdI, (unsigned long)cfg.allowedBauds[spdI]);
    if (isMaster) {
        sendFrame_unlocked(PING_CMD);
        hw.startTimer(dwells_.phase1);
    } else {
        hw.startTimer(dwells_.phase1 *
                      PHASE1_MAX_TRIES);
    }
}

void Link::enterPhase2_unlocked()
{
    sweepPhase_ = SweepPhase::PHASE2;
    spdI = 0;
    pingSample = 0;
    hw.setSpd(cfg.allowedBauds[spdI]);
    Log::log().info(
        ALINK_TAG,
        "=== PHASE 2: top-down sweep, 1 ack/baud ===");
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
    Log::log().info(
        ALINK_TAG,
        "=== PHASE 3: 2-of-3 at baud[%d]=%lu ===",
        chosenBaud,
        (unsigned long)cfg.allowedBauds[chosenBaud]);
    if (isMaster) {
        sendFrame_unlocked(PING_CMD);
        int rt =
            (int)(2.0 *
                      (5.0 * 10.0 /
                       cfg.allowedBauds[chosenBaud] *
                       1000.0) +
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
    if (cfg.baudPreference &&
        preferredBaud_ != NO_PREFERRED_BAUD &&
        preferredBaud_ < (int)cfg.allowedBaudsCount) {
        if (baudRetries_ < cfg.baudRetryLimit) {
            spdI = preferredBaud_;
            baudRetries_++;
            Log::log().info(
                ALINK_TAG,
                "=== RESWEEP: preferred baud[%d]=%lu "
                "(retry %d/%d) ===",
                spdI,
                (unsigned long)cfg.allowedBauds[spdI],
                baudRetries_, cfg.baudRetryLimit);
        } else {
            spdI = 0;
            preferredBaud_ = NO_PREFERRED_BAUD;
            baudRetries_ = 0;
            Log::log().info(ALINK_TAG,
                            "=== RESWEEP: full sweep "
                            "from baud[0] ===");
        }
    } else {
        spdI = 0;
    }
    pingSample = 0;
    hw.setSpd(cfg.allowedBauds[spdI]);
    if (isMaster) {
        hw.startTimer(dwells_.phase2[spdI]);
    } else {
        hw.startTimer(dwells_.phase2Slave[spdI]);
    }
}

void Link::sendFrame_unlocked(uint8_t payload)
{
    uint8_t frame[CTRL_FRAME_SIZE];
    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[CTRL_FRAME_SEQ_IDX] = txSeq;
    frame[CTRL_FRAME_PAYLOAD_IDX] = payload;
    frame[CTRL_FRAME_CRC_IDX] =
        UtilCrc::crc8(frame, CTRL_FRAME_SIZE - 1);
    if (hw.tx(frame, CTRL_FRAME_SIZE) !=
        CTRL_FRAME_SIZE) {
        Log::log().error(ALINK_TAG,
                         "sendFrame TX truncated "
                         "(cobsSeq=%u payload=0x%02X)",
                         (unsigned)txSeq,
                         (unsigned)payload);
    }
    Log::log().verbose(
        ALINK_TAG,
        "sendFrame TX  cobsSeq=%u  payload=0x%02X",
        (unsigned)txSeq, (unsigned)payload);
}

void Link::sendFrame(uint8_t payload)
{
    hw.lock();
    sendFrame_unlocked(payload);
    hw.unlock();
}

void Link::sendCobsFrame_unlocked(const uint8_t *b,
                                  int n)
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
    size_t encLen =
        UtilCobs::encode(unenc, rawLen, frame + 1);
    frame[1 + encLen] = 0x00;
    size_t frameLen = encLen + 2;

    int sent = hw.tx(frame, (int)frameLen);
    if ((int)frameLen != sent) {
        Log::log().error(
            ALINK_TAG,
            "sendCobsFrame TX truncated (cobsSeq=%u "
            "len=%d accepted=%d)",
            (unsigned)txSeq, (int)frameLen, sent);
    } else {
        Log::log().debug(ALINK_TAG,
                         "TX cobsSeq=%u  %d payload "
                         "bytes  %d wire bytes",
                         (unsigned)txSeq, n,
                         (int)frameLen);
    }


    txSeq = (txSeq == COBS_SEQ_MAX)
        ? 0
        : (uint8_t)(txSeq + 1);
    // Skip both 0xFE and 0xFF: wire discriminators
    // (NAK/ACK), not data seq.
}

void Link::sendCobsFrame(const uint8_t *b, int n)
{
    hw.lock();
    sendCobsFrame_unlocked(b, n);
    hw.unlock();
}

uint8_t Link::sendCobsFrameAcked_unlocked(
    const uint8_t *b, int n, uint8_t baseSeq)
{
    uint8_t seq = txSeq;
    sendCobsFrame_unlocked(b, n);
    ackedPending_[seq] = true;
    retxCount_[seq] = 0;
    sentAtMs_[seq] = hw.nowMs();
    baseSeq_[seq] =
        (baseSeq == NO_BASE) ? seq : baseSeq;


    if (arqCacheInsertCallback_ && n > 0) {
        arqCacheInsertCallback_(seq, b, n, (uint8_t)1,
                                arqCtx_);
    }
    return seq;
}

void Link::resendCobsFrame_unlocked(uint8_t seq,
                                    const uint8_t *b,
                                    int n)
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
    size_t encLen =
        UtilCobs::encode(unenc, rawLen, frame + 1);
    frame[1 + encLen] = 0x00;
    int sent = hw.tx(frame, (int)(encLen + 2));
    if ((int)(encLen + 2) != sent) {
        Log::log().error(
            ALINK_TAG,
            "resendCobsFrame TX truncated (cobsSeq=%u "
            "len=%d accepted=%d)",
            (unsigned)seq, (int)(encLen + 2), sent);
    }
    Log::log().debug(ALINK_TAG,
                     "RETX cobsSeq=%u  %d payload "
                     "bytes  %d wire bytes",
                     (unsigned)seq, n,
                     (int)(encLen + 2));
}

void Link::sendAckFrame_unlocked(uint8_t ackedCobsSeq)
{
    uint8_t unenc[3] = { ACK_TYPE, ackedCobsSeq, 0 };
    unenc[2] = UtilCrc::crc8(unenc, 2);
    uint8_t frame[8];
    frame[0] = 0x00;
    size_t encLen =
        UtilCobs::encode(unenc, 3, frame + 1);
    frame[1 + encLen] = 0x00;
    int sent = hw.tx(frame, (int)(encLen + 2));
    if ((int)(encLen + 2) != sent) {
        Log::log().error(ALINK_TAG,
                         "sendAckFrame TX truncated "
                         "(ackedSeq=%u accepted=%d)",
                         (unsigned)ackedCobsSeq, sent);
    }
}

void Link::sendNakFrame_unlocked(
    uint8_t missingCobsSeq)
{
    uint8_t unenc[3] = { NAK_TYPE, missingCobsSeq, 0 };
    unenc[2] = UtilCrc::crc8(unenc, 2);
    uint8_t frame[8];
    frame[0] = 0x00;
    size_t encLen =
        UtilCobs::encode(unenc, 3, frame + 1);
    frame[1 + encLen] = 0x00;
    int sent = hw.tx(frame, (int)(encLen + 2));
    if ((int)(encLen + 2) != sent) {
        Log::log().error(ALINK_TAG,
                         "sendNakFrame TX truncated "
                         "(missingSeq=%u accepted=%d)",
                         (unsigned)missingCobsSeq,
                         sent);
    }
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
    Log::log().debug(
        ALINK_TAG,
        "frame error #%d (cumulative frameErrs=%llu, "
        "threshold=%d, rate=%d/s, rateLimit=%d)",
        errs, (unsigned long long)frameErrs,
        cfg.errThreshold, errWindowCount_,
        cfg.errRateWindow);
    if (errs > cfg.errThreshold) {
        Log::log().info(ALINK_TAG,
                        "Error threshold exceeded (%d "
                        "> %d). Dropping link.",
                        errs, cfg.errThreshold);
        reset_unlocked(true);
        return true;
    }
    if (cfg.errRateWindow > 0 &&
        errWindowCount_ > cfg.errRateWindow) {
        Log::log().warning(
            ALINK_TAG,
            "Error rate exceeded (%d in last 1 s, "
            "limit %d). Re-sweeping.",
            errWindowCount_, cfg.errRateWindow);
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
    if (len <= 0) {
        if (len < 0) {
            Log::log().error(
                ALINK_TAG,
                "write rejected: len=%d (negative). "
                "No bytes sent.",
                len);
        }
        return 0;
    }
    hw.lock();
    int sent = sendMsg_unlocked(b, len);
    hw.unlock();
    return sent;
}

int Link::sendMsg_unlocked(const uint8_t *b, int len)
{
    if (state != State::OK) {
        State s = state;
        Log::log().warning(
            ALINK_TAG,
            "write rejected: link not in OK "
            "(state=%s), %d bytes dropped. "
            "Call dropLink() and wait for re-sweep, "
            "or check wiring/peer.",
            StateToStr(s), len);
        return 0;
    }

    int offset = 0;
    while (offset < len) {
        if (state != State::OK) {
            State s = state;
            Log::log().warning(
                ALINK_TAG,
                "write aborted mid-message at "
                "offset=%d/%d (state=%s). "
                "Peer will see a gap; the partial "
                "message is lost.",
                offset, len, StateToStr(s));
            break;
        }
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
    bool needBreak = (state == State::SWP);
    hw.unlock();
    if (needBreak)
        hw.sendBreak();
}

void Link::flush()
{
    hw.flushTx();
}

void Link::flushRx()
{
    hw.lock();
    int app_bytes = hw.appBufAvailable();
    hw.clearAppBuf();
    rxMsgLen = -1;


    rxSeqSet = false;
    rxSeq = 0;
    hw.unlock();
    hw.flushRxHw();
    Log::log().debug(
        ALINK_TAG,
        "flushRx: cleared %d stream bytes + hw ring "
        "(rxMsgLen + cobsSeq rx state reset)",
        app_bytes);
}

bool Link::sendMsg(const uint8_t *b, int len,
                   uint8_t *outBaseSeq)
{
    if (len == 0) {
        if (outBaseSeq)
            *outBaseSeq = 0;
        return true;
    }
    if (len < 0) {
        Log::log().error(
            ALINK_TAG,
            "sendMsg rejected: len=%d (negative).",
            len);
        if (outBaseSeq)
            *outBaseSeq = 0;
        return false;
    }
    if ((size_t)len > cfg.maxMsg) {
        Log::log().error(ALINK_TAG,
                         "sendMsg rejected: len=%d "
                         "exceeds cfg.maxMsg=%u.",
                         len, (unsigned)cfg.maxMsg);
        if (outBaseSeq)
            *outBaseSeq = 0;
        return false;
    }

    uint16_t c = UtilCrc::crc16(b, len);
    uint8_t hdr[MSG_HDR] = {
        (uint8_t)(len),       (uint8_t)(len >> 8),
        (uint8_t)(len >> 16), (uint8_t)(len >> 24),
        (uint8_t)(c),         (uint8_t)(c >> 8)
    };

    hw.lock();
    if (state != State::OK) {
        State s = state;
        hw.unlock();
        Log::log().warning(
            ALINK_TAG,
            "sendMsg rejected: link not in OK "
            "(state=%s), %d bytes dropped.",
            StateToStr(s), len);
        if (outBaseSeq)
            *outBaseSeq = 0;
        return false;
    }


    if (arqCacheHasRoomCallback_ &&
        !arqCacheHasRoomCallback_(arqCtx_)) {
        hw.unlock();
        Log::log().warning(
            ALINK_TAG,
            "sendMsg rejected: ARQ cache full, "
            "back-pressure. %d bytes dropped.",
            len);
        if (outBaseSeq)
            *outBaseSeq = 0;
        return false;
    }

    bool ok = true;
    uint8_t baseSeq = 0;


    if (len + MSG_HDR <= MAX_CHUNK) {
        // Header+data coalesce for short messages: one
        // wire frame instead of two.
        uint8_t merged[MAX_CHUNK];
        memcpy(merged, hdr, MSG_HDR);
        if (len > 0)
            memcpy(merged + MSG_HDR, b, len);
        baseSeq = sendCobsFrameAcked_unlocked(
            merged, MSG_HDR + len, NO_BASE);
        txBytes += len;
        lastTxMs = hw.nowMs();
    } else {
        baseSeq = sendCobsFrameAcked_unlocked(
            hdr, MSG_HDR, NO_BASE);
        int offset = 0;
        while (offset < len) {
            if (state != State::OK) {
                ok = false;
                break;
            }
            int chunk =
                std::min(len - offset, MAX_CHUNK);
            sendCobsFrameAcked_unlocked(
                b + offset, chunk, baseSeq);
            txBytes += chunk;
            lastTxMs = hw.nowMs();
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
    int scan = (max_scan > 0 && max_scan < avail)
        ? max_scan
        : avail;
    int snapLen = scan + MSG_HDR;
    if (snapLen > avail)
        snapLen = avail;
    uint8_t *snap = (uint8_t *)malloc(snapLen);
    if (!snap)
        return -1;
    int got = hw.popAppBuf(snap, snapLen);
    int validLen = got;
    Log::log().debug(ALINK_TAG,
                     "findMsgHeaderResync: avail=%d "
                     "snapLen=%d got=%d validLen=%d",
                     avail, snapLen, got, validLen);
    for (int drop = 0; drop + MSG_HDR <= validLen;
         drop++) {
        uint32_t L = (uint32_t)snap[drop] |
            ((uint32_t)snap[drop + 1] << 8) |
            ((uint32_t)snap[drop + 2] << 16) |
            ((uint32_t)snap[drop + 3] << 24);
        if (L < 1 || L > cfg.maxMsg)
            continue;
        int totalNeeded = (int)L + 6;
        if (drop + totalNeeded > validLen)
            continue;
        uint16_t wantCrc = (uint16_t)snap[drop + 4] |
            ((uint16_t)snap[drop + 5] << 8);
        uint16_t gotCrc =
            UtilCrc::crc16(snap + drop + 6, (int)L);
        if (gotCrc != wantCrc) {
            Log::log().debug(
                ALINK_TAG,
                "findMsgHeaderResync: candidate "
                "drop=%d L=%u CRC mismatch "
                "(want=0x%04X got=0x%04X) — skipping "
                "(boundary check)",
                drop, (unsigned)L, (unsigned)wantCrc,
                (unsigned)gotCrc);
            continue;
        }
        Log::log().debug(
            ALINK_TAG,
            "findMsgHeaderResync: candidate drop=%d "
            "L=%u CRC ok",
            drop, (unsigned)L);
        hw.pushAppBuf(snap + drop, validLen - drop);
        free(snap);
        return drop;
    }
    hw.pushAppBuf(snap, validLen);
    free(snap);
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
        uint32_t L = (uint32_t)h[0] |
            ((uint32_t)h[1] << 8) |
            ((uint32_t)h[2] << 16) |
            ((uint32_t)h[3] << 24);
        rxMsgCrc =
            (uint16_t)h[4] | ((uint16_t)h[5] << 8);
        if (L == 0 || L > cfg.maxMsg) {
            int drop = findMsgHeaderResync_unlocked(
                cfg.maxMsg + MSG_HDR);
            if (drop >= 0) {
                if (drop > 0) {
                    Log::log().error(
                        ALINK_TAG,
                        "recvMsg: corrupt MSG_HDR "
                        "(L=%u) — resynced forward "
                        "by %d bytes to the next "
                        "valid header. Lost %d bytes "
                        "of stream.",
                        (unsigned)L, drop, drop);
                    hw.unlock();
                    err();
                    return -1;
                }
                Log::log().debug(
                    ALINK_TAG,
                    "recvMsg: corrupt MSG_HDR (L=%u) "
                    "at the very start of "
                    "the scan window — the bytes just "
                    "past it form a "
                    "plausible L. Returning -1; the "
                    "next recvMsg will "
                    "re-read from there.",
                    (unsigned)L);
                hw.unlock();
                return -1;
            }
            int cleared = hw.appBufAvailable();
            hw.clearAppBuf();
            rxMsgLen = -1;
            Log::log().error(
                ALINK_TAG,
                "recvMsg: corrupt MSG_HDR (L=%u) and "
                "no resync point found "
                "within %d bytes — cleared the app "
                "buffer (%d bytes dropped). "
                "Link stays OK; the next sendMsg from "
                "the peer will be "
                "received cleanly.",
                (unsigned)L, cfg.maxMsg + MSG_HDR,
                cleared);
            hw.unlock();
            if (cleared > 0)
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
    uint16_t expectedCrc = rxMsgCrc;
    rxMsgLen = -1;

    if (len > max_len) {
        uint8_t sink[256];
        int left = len;
        while (left > 0)
            left -= readStream(
                sink,
                std::min(left, (int)sizeof(sink)));
        hw.unlock();
        err();
        return -1;
    }

    readStream(out, len);
    bool ok =
        (UtilCrc::crc16(out, len) == expectedCrc);
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
    txBytes = 0;
    rxBytes = 0;
    hw.unlock();
}
void Link::resetErrors()
{
    hw.lock();
    discCount = 0;
    frameErrs = 0;
    errWindowCount_ = 0;
    errWindowStartMs_ = hw.nowMs();
    hw.unlock();
}

void Link::resetDiag()
{
    hw.lock();
    gaps = 0;
    stale = 0;
    lostMsgs = 0;
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
    int idx = spdI;
    hw.unlock();
    return idx;
}
uint32_t Link::getCurrentBaud() const
{
    hw.lock();
    uint32_t b = (spdI >= 0 &&
                  spdI < (int)cfg.allowedBaudsCount)
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
    // Record the locked baud so reset_unlocked honors
    // baudPreference (start re-sweep at
    // preferredBaud_, full sweep after retry cap).
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
    Log::log().info(
        ALINK_TAG,
        "Locked at %lu baud (%s, preferred=%lu)",
        (unsigned long)cfg.allowedBauds[idx], tag,
        (unsigned long)
            cfg.allowedBauds[preferredBaud_]);
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
    bool needSendBreak = false;
    while (i < len) {
        State curState = state;

        if (curState == State::OK) {
            int start = i;
            while (i < len) {
                uint8_t b = data[i];
                if (b == 0xAA && (len - i) >= 2 &&
                    data[i + 1] == 0x55) {
                    if (i > start) {
                        int consumed = frameRx.feed(
                            data + start, i - start);
                        if (state != State::OK) {
                            i = start + consumed;
                            break;
                        }
                    }
                    if (len - i >= CTRL_FRAME_SIZE) {
                        for (int k = 0;
                             k < CTRL_FRAME_SIZE;
                             k++) {
                            rxBuf[k] = data[i + k];
                        }
                        i += CTRL_FRAME_SIZE;
                        rxIdx = 0;
                        if (UtilCrc::crc8(
                                rxBuf,
                                CTRL_FRAME_SIZE - 1) !=
                            rxBuf
                                [CTRL_FRAME_CRC_IDX]) {
                            Log::log().debug(
                                ALINK_TAG,
                                "OK ctrl frame bad "
                                "CRC");
                            if (err_unlocked())
                                needSendBreak = true;
                        } else {
                            uint8_t cs = rxBuf
                                [CTRL_FRAME_SEQ_IDX];
                            uint8_t pl = rxBuf
                                [CTRL_FRAME_PAYLOAD_IDX];
                            if (pl == PING_CMD) {
                                sendPongAck_unlocked();
                                Log::log().verbose(
                                    ALINK_TAG,
                                    "OK heartbeat "
                                    "PING cobsSeq=%u "
                                    "-> PONG_ACK",
                                    (unsigned)cs);
                            } else if (pl ==
                                       PONG_CMD) {
                                heartbeatPingsMissed_ =
                                    0;
                                Log::log().verbose(
                                    ALINK_TAG,
                                    "OK heartbeat "
                                    "PONG_ACK "
                                    "cobsSeq=%u",
                                    (unsigned)cs);
                            }
                        }
                        start = i;
                    } else {
                        int consumed = frameRx.feed(
                            data + start, len - start);
                        i = start + consumed;
                        start = i;
                        break;
                    }
                } else {
                    i++;
                }
            }
            if (i > start && state == State::OK) {
                int consumed = frameRx.feed(
                    data + start, i - start);
                i = start + consumed;
            }
            if (state != State::OK)
                needSendBreak = true;
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
                if (UtilCrc::crc8(
                        rxBuf, CTRL_FRAME_SIZE - 1) !=
                    rxBuf[CTRL_FRAME_CRC_IDX]) {
                    Log::log().debug(
                        ALINK_TAG,
                        "control frame bad CRC: %02X "
                        "%02X %02X %02X %02X",
                        rxBuf[0], rxBuf[1], rxBuf[2],
                        rxBuf[3], rxBuf[4]);
                    if (err_unlocked())
                        needSendBreak = true;
                    continue;
                }
                uint8_t cobsSeq =
                    rxBuf[CTRL_FRAME_SEQ_IDX];
                uint8_t payload =
                    rxBuf[CTRL_FRAME_PAYLOAD_IDX];
                if (ctrlFrameReady_unlocked(
                        cobsSeq, payload, curState)) {
                    needSendBreak = true;
                }
            }
        }
    }
    hw.unlock();
    if (needSendBreak)
        hw.sendBreak();
}

bool Link::ctrlFrameReady_unlocked(uint8_t cobsSeq,
                                   uint8_t payload,
                                   State curState)
{
    if (curState == State::SWP)
        return handleSwp_unlocked(cobsSeq, payload);
    if (curState == State::LCK)
        return handleLck_unlocked(cobsSeq, payload);
    return false;
}

bool Link::handleSwp_unlocked(uint8_t cobsSeq,
                              uint8_t payload)
{
    (void)cobsSeq;
    if (!isMaster && payload == REQ_CMD) {
        int best = bestSpd_unlocked();
        sendFrame_unlocked((uint8_t)best);
        lockOk_unlocked(best, "REQ");
        return false;
    }


    if (payload >= LOCK_CMD_BASE &&
        payload <
            LOCK_CMD_BASE + cfg.allowedBaudsCount) {
        // LOCK payload encodes baud index; pong's
        // RX-driven confirmation. Master is RX-driven
        // too: waits 2-of-3 at the chosen baud.
        int lockedBaud =
            (int)(payload - LOCK_CMD_BASE);
        Log::log().info(
            ALINK_TAG,
            "LOCK signal -> confirm at baud[%d]=%lu",
            lockedBaud,
            (unsigned long)
                cfg.allowedBauds[lockedBaud]);


        if (sweepPhase_ == SweepPhase::PHASE3) {
            sweepPhase_ = SweepPhase::NONE;
            phase3Baud_ = -1;
            phase3Acks_ = 0;
            hw.setSpd(cfg.allowedBauds[lockedBaud]);
            spdI = lockedBaud;
            lockOk_unlocked(lockedBaud,
                            isMaster
                                ? "phase3-lock"
                                : "phase3-lock-pong");
            return false;
        }
        if (sweepPhase_ == SweepPhase::PHASE2) {
            hw.setSpd(cfg.allowedBauds[lockedBaud]);
            spdI = lockedBaud;
            sweepPhase_ = SweepPhase::NONE;
            phase3Baud_ = -1;
            phase3Acks_ = 0;
            lockOk_unlocked(lockedBaud, "phase2-lock");
            return false;
        }


        hw.setSpd(cfg.allowedBauds[lockedBaud]);
        spdI = lockedBaud;
        sweepPhase_ = SweepPhase::NONE;
        phase3Baud_ = -1;
        phase3Acks_ = 0;
        lockOk_unlocked(lockedBaud, "phase1-lock");
        return false;
    }

    if (payload == PONG_CMD) {
        if (isMaster) {
            if (sweepPhase_ == SweepPhase::PHASE1) {
                Log::log().info(
                    ALINK_TAG,
                    "phase1: PONG_ACK@baud[%d]=%lu "
                    "cobsSeq=%u -> LOCK at slowest "
                    "baud",
                    spdI,
                    (unsigned long)
                        cfg.allowedBauds[spdI],
                    (unsigned)cobsSeq);
                int lockedBaud = spdI;
                sweepPhase_ = SweepPhase::NONE;
                phase3Baud_ = -1;
                phase3Acks_ = 0;
                hw.setSpd(
                    cfg.allowedBauds[lockedBaud]);
                spdI = lockedBaud;
                sendFrame_unlocked(
                    LOCK_CMD + (uint8_t)lockedBaud);
                lockOk_unlocked(lockedBaud, "phase1");
                return false;
            }
            if (sweepPhase_ == SweepPhase::PHASE2) {
                int chosen = spdI;
                Log::log().info(
                    ALINK_TAG,
                    "phase2: PONG_ACK@baud[%d]=%lu "
                    "cobsSeq=%u -> PROMOTE to PHASE 3 "
                    "(P2 short-circuit)",
                    chosen,
                    (unsigned long)
                        cfg.allowedBauds[chosen],
                    (unsigned)cobsSeq);
                enterPhase3_unlocked(chosen);
                return false;
            }
            if (sweepPhase_ == SweepPhase::PHASE3) {
                phase3Acks_++;
                Log::log().info(ALINK_TAG,
                                "phase3: PONG_ACK "
                                "%d/%d cobsSeq=%u",
                                phase3Acks_,
                                PHASE3_ACKS_NEEDED,
                                (unsigned)cobsSeq);
                if (phase3Acks_ >=
                    PHASE3_ACKS_NEEDED) {
                    Log::log().info(
                        ALINK_TAG,
                        "phase3: %d/%d acks -> "
                        "CONFIRMED at baud[%d]=%lu",
                        phase3Acks_,
                        PHASE3_ACKS_NEEDED,
                        phase3Baud_,
                        (unsigned long)cfg.allowedBauds
                            [phase3Baud_]);
                    int lockedBaud = phase3Baud_;
                    sweepPhase_ = SweepPhase::NONE;
                    phase3Baud_ = -1;
                    phase3Acks_ = 0;
                    if (isMaster) {
                        hw.setSpd(cfg.allowedBauds
                                      [lockedBaud]);
                        spdI = lockedBaud;
                        sendFrame_unlocked(
                            LOCK_CMD +
                            (uint8_t)lockedBaud);
                    }
                    lockOk_unlocked(lockedBaud,
                                    "phase3");
                    return false;
                }


                sendFrame_unlocked(PING_CMD);
                int rt = (int)(2.0 *
                                   (5.0 * 10.0 /
                                    cfg.allowedBauds
                                        [phase3Baud_] *
                                    1000.0) +
                               0.5);
                if (rt < 5)
                    rt = 5;
                int t3 = rt *
                        (PHASE3_ACKS_NEEDED -
                         phase3Acks_ + 1) +
                    100;
                if (t3 < 200)
                    t3 = 200;
                hw.startTimer(t3);
                heartbeatPingsMissed_ = 0;
                return false;
            }
        } else {
            return false;
        }
    }

    if (payload == PING_CMD) {
        if (!isMaster) {
            baudSweep.score(spdI);


            if (sweepPhase_ == SweepPhase::PHASE1) {
                Log::log().info(
                    ALINK_TAG,
                    "pong: phase1 PING at "
                    "baud[%d]=%lu cobsSeq=%u -> "
                    "PONG_ACK",
                    spdI,
                    (unsigned long)
                        cfg.allowedBauds[spdI],
                    (unsigned)cobsSeq);
                sendPongAck_unlocked();
                return false;
            } else if (sweepPhase_ ==
                       SweepPhase::PHASE2) {
                Log::log().info(
                    ALINK_TAG,
                    "pong: phase2 PING at "
                    "baud[%d]=%lu cobsSeq=%u -> PHASE "
                    "3",
                    spdI,
                    (unsigned long)
                        cfg.allowedBauds[spdI],
                    (unsigned)cobsSeq);
                sweepPhase_ = SweepPhase::PHASE3;
                phase3Baud_ = spdI;
                phase3Acks_ = 0;
                hw.setSpd(
                    cfg.allowedBauds[phase3Baud_]);
            } else if (sweepPhase_ ==
                       SweepPhase::PHASE3) {
                phase3Acks_++;
                Log::log().info(
                    ALINK_TAG,
                    "pong: phase3 PONG_ACK %d/%d at "
                    "baud[%d]=%lu",
                    phase3Acks_, PHASE3_ACKS_NEEDED,
                    phase3Baud_,
                    (unsigned long)
                        cfg.allowedBauds[phase3Baud_]);
                if (phase3Acks_ >=
                    PHASE3_ACKS_NEEDED) {
                    Log::log().info(
                        ALINK_TAG,
                        "pong: phase3 %d/%d acks -> "
                        "CONFIRMED at baud[%d]=%lu",
                        phase3Acks_,
                        PHASE3_ACKS_NEEDED,
                        phase3Baud_,
                        (unsigned long)cfg.allowedBauds
                            [phase3Baud_]);
                    int lockedBaud = phase3Baud_;
                    sweepPhase_ = SweepPhase::NONE;
                    phase3Baud_ = -1;
                    phase3Acks_ = 0;
                    hw.setSpd(
                        cfg.allowedBauds[lockedBaud]);
                    spdI = lockedBaud;


                    sendFrame_unlocked(
                        LOCK_CMD +
                        (uint8_t)lockedBaud);
                    lockOk_unlocked(lockedBaud,
                                    "phase3-pong");
                    return false;
                }
            }
            sendPongAck_unlocked();
            Log::log().verbose(
                ALINK_TAG,
                "pong: PING@baud[%d]=%lu cobsSeq=%u "
                "-> PONG_ACK",
                spdI,
                (unsigned long)cfg.allowedBauds[spdI],
                (unsigned)cobsSeq);
            heartbeatPingsMissed_ = 0;
            return false;
        } else {
            return false;
        }
    }

    return false;
}

bool Link::handleLck_unlocked(uint8_t cobsSeq,
                              uint8_t payload)
{
    (void)cobsSeq;
    if (isMaster) {
        if (payload < (int)cfg.allowedBaudsCount) {
            lockOk_unlocked((int)payload, "REQ");
        }
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
    // Policy says: any break or line error → start
    // over at P1 slowest baud. 3-phase sweep is fast
    // on a clean wire (~50 ms), so honoring
    // preferredBaud_ would risk locking on a baud that
    // just failed.
    bool hadPreferred =
        (preferredBaud_ != NO_PREFERRED_BAUD);
    ResetAction act =
        decideResetPolicy(hadPreferred, baudRetries_,
                          cfg.baudRetryLimit);
    (void)act;
    spdI = 0;
    preferredBaud_ = NO_PREFERRED_BAUD;
    baudRetries_ = 0;
    pingSample = 0;
    rxIdx = 0;
    rxMsgLen = -1;
    frameRx.reset();
    baudSweep.resetAll();
    errs = 0;
    lckRetries = 0;
    emptySweeps = 0;
    swpRxBytes = 0;
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

bool Link::onPayload(uint8_t cobsSeq, const uint8_t *b,
                     int n)
{
    if (state != State::OK) {
        Log::log().debug(
            ALINK_TAG,
            "RX cobsSeq=%u  %d payload bytes  DROPPED "
            "(state != OK)",
            (unsigned)cobsSeq, n);
        return true;
    }


    reorderDropExpired_unlocked(hw.nowMs());

    int diff = 0;
    GapClass cls =
        classifyGap(cobsSeq, rxSeq, rxSeqSet, &diff);
    if (cls == GapClass::Stale) {
        stale++;
        Log::log().debug(
            ALINK_TAG,
            "RX cobsSeq=%u STALE: expected %u, last "
            "good=%u  %d bytes DROPPED "
            "(duplicate retransmit)",
            (unsigned)cobsSeq,
            (unsigned)((uint8_t)(rxSeq + 1)),
            (unsigned)rxSeq, n);
        return false;
    }
    if (cls == GapClass::Gap) {
        if (cfg._test_forwardResync) {
            (void)((uint8_t)((rxSeq ==
                              (uint8_t)(COBS_SEQ_MAX))
                                 ? 0
                                 : (rxSeq + 1)));
            gaps++;
            uint64_t skipped = (uint64_t)(diff - 1);
            lostMsgs += skipped + 1;
            Log::log().warning(
                ALINK_TAG,
                "RX cobsSeq=%u GAP: TEST-MODE "
                "forward-resync (will drop "
                "retransmit)",
                (unsigned)cobsSeq);
            rxSeq = cobsSeq;
            rxSeqSet = true;
            if (n > 0) {
                hw.pushAppBuf(b, n);
                rxBytes += n;
            }
            sendAckFrame_unlocked(cobsSeq);
            return false;
        }


        uint8_t expected =
            (uint8_t)((rxSeq ==
                       (uint8_t)(COBS_SEQ_MAX))
                          ? 0
                          : (rxSeq + 1));
        gaps++;
        uint64_t skipped = (uint64_t)(diff - 1);


        Log::log().info(
            ALINK_TAG,
            "RX cobsSeq=%u GAP: expected %u, last "
            "good=%u  %d bytes HELD (reorder[%u], "
            "gap=%d, NAK %u -> fast retransmit)",
            (unsigned)cobsSeq, (unsigned)expected,
            (unsigned)rxSeq, n, (unsigned)cobsSeq,
            diff, (unsigned)expected);
        if (n > 0) {
            uint8_t *slotBuf =
                (uint8_t *)malloc((size_t)n);
            if (!slotBuf) {
                Log::log().error(
                    ALINK_TAG,
                    "RX cobsSeq=%u reorder malloc(%d) "
                    "FAILED — dropping held frame + "
                    "marking gap as lost",
                    (unsigned)cobsSeq, n);
                lostMsgs += skipped + 1;
                rxSeq = cobsSeq;
                rxSeqSet = true;
                return false;
            }
            memcpy(slotBuf, b, (size_t)n);


            if (reorder_[cobsSeq].in_use) {
                free(reorder_[cobsSeq].buf);
                lostMsgs++;
                Log::log().warning(
                    ALINK_TAG,
                    "RX cobsSeq=%u reorder slot "
                    "already held — overwriting prior "
                    "held frame",
                    (unsigned)cobsSeq);
            }
            reorder_[cobsSeq].buf = slotBuf;
            reorder_[cobsSeq].len = (uint16_t)n;
            reorder_[cobsSeq].heldAtMs = hw.nowMs();
            reorder_[cobsSeq].in_use = true;
        } else {
            if (reorder_[cobsSeq].in_use)
                free(reorder_[cobsSeq].buf);
            reorder_[cobsSeq].buf = nullptr;
            reorder_[cobsSeq].len = 0;
            reorder_[cobsSeq].heldAtMs = hw.nowMs();
            reorder_[cobsSeq].in_use = true;
        }
        sendNakFrame_unlocked(expected);
        return false;
    }
    rxSeq = cobsSeq;
    rxSeqSet = true;


    if (n == 0) {
        Log::log().debug(
            ALINK_TAG,
            "RX cobsSeq=%u keepalive (0 payload "
            "bytes) — no push, no ACK",
            (unsigned)cobsSeq);
        if (errs > 0)
            errs = 0;
        return false;
    }

    int acc = hw.pushAppBuf(b, n);
    rxBytes += acc;
    AppBufAction appAction = decideAppBuf(acc, n);
    if (appAction == AppBufAction::HoldAck ||
        acc < n) {
        Log::log().info(
            ALINK_TAG,
            "RX cobsSeq=%u app buffer full: wanted %d "
            "accepted %d (frame NOT acked, sender "
            "will "
            "retransmit). "
            "This is flow control, not a wire error — "
            "gaps/errs not bumped. "
            "If this fires on the FIRST frame, check "
            "the EspHal boot log for "
            "xStreamBufferCreate failure (heap too "
            "small or streamBufferSize too large).",
            (unsigned)cobsSeq, n, acc);
        return true;
    }
    sendAckFrame_unlocked(cobsSeq);
    Log::log().debug(ALINK_TAG,
                     "RX cobsSeq=%u  %d payload bytes "
                     " acked (rxBytes=%llu)",
                     (unsigned)cobsSeq, n,
                     (unsigned long long)rxBytes);


    reorderFlushContiguous_unlocked(hw.nowMs());
    if (n > 0) {
        int hd = n < 10 ? n : 10;
        char hex[10 * 3 + 1] = {};
        for (int i = 0; i < hd; i++) {
            snprintf(hex + i * 3, 4, "%02X ", b[i]);
        }
        Log::log().verbose(
            ALINK_TAG, "RX hex first %d: %s", hd, hex);
    }
    if (errs > 0)
        errs = 0;
    return false;
}

bool Link::onAck(uint8_t ackedCobsSeq)
{
    if (state != State::OK)
        return false;
    if (!ackedPending_[ackedCobsSeq]) {
        Log::log().debug(
            ALINK_TAG,
            "RX ACK for cobsSeq=%u DROPPED (not in "
            "pending map)",
            (unsigned)ackedCobsSeq);
        return false;
    }
    ackedPending_[ackedCobsSeq] = false;
    retxCount_[ackedCobsSeq] = 0;
    baseSeq_[ackedCobsSeq] = 0;
    Log::log().verbose(ALINK_TAG,
                       "RX ACK cobsSeq=%u  slot freed",
                       (unsigned)ackedCobsSeq);
    if (arqAckCallback_)
        arqAckCallback_(ackedCobsSeq, arqCtx_);
    return false;
}

bool Link::onNak(uint8_t missingCobsSeq)
{
    if (state != State::OK)
        return false;
    if (!ackedPending_[missingCobsSeq]) {
        Log::log().debug(
            ALINK_TAG,
            "RX NAK for cobsSeq=%u DROPPED (not in "
            "pending map)",
            (unsigned)missingCobsSeq);
        return false;
    }
    sentAtMs_[missingCobsSeq] = hw.nowMs();
    pendingRetxBase_ = missingCobsSeq;
    hasPendingRetx_ = true;
    retxNeeded_ = true;
    Log::log().info(ALINK_TAG,
                    "RX NAK cobsSeq=%u -> fast "
                    "retransmit (deferred to onTimer)",
                    (unsigned)missingCobsSeq);
    return false;
}

bool Link::onFrameError()
{
    Log::log().debug(ALINK_TAG,
                     "frame error (corrupt COBS / bad "
                     "CRC / oversize)");
    return err_unlocked();
}

void Link::onBreak()
{
    hw.lock();
    Log::log().info(ALINK_TAG,
                    "BREAK received -> re-sweep");
    reset_unlocked(true);
    hw.unlock();
}

void Link::onTimer()
{
    State s;
    int curSpd;
    hw.lock();
    s = state;
    curSpd = spdI;

    if (s == State::OK)
        onTimerOk_unlocked();
    else if (s == State::SWP)
        onTimerSwp_unlocked();
    else if (s == State::LCK && isMaster)
        onTimerLck_unlocked();
    else {
        hw.unlock();
        (void)curSpd;
        return;
    }
    hw.unlock();


    if (hasPendingRetx_ && arqRetxCallback_) {
        // arqRetxCallback_ return: true = drop request
        // (link should reset), false = retransmit
        // succeeded / was a no-op. Never treat a
        // successful retransmit as a reason to drop.
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
    // Sole authority on ARQ drops: MAX_RETX exhaustion
    // in the per-slot loop below. Resending must NOT
    // trigger a drop.
    if (cfg.idleTimeoutMs <= 0)
        return;
    uint32_t now = hw.nowMs();
    if (linkPaused_)
        return;


    reorderDropExpired_unlocked(now);
    {
        uint32_t rxAge = now - lastRxMs;
        uint32_t txAge = now - lastTxMs;


        if (rxAge > (uint32_t)FAST_IDLE_RX_MS &&
            txAge < (uint32_t)FAST_IDLE_TX_MS) {
            Log::log().warning(
                ALINK_TAG,
                "Asymmetric idle (rxAge=%u ms, "
                "txAge=%u ms) -> drop + resweep",
                (unsigned)rxAge, (unsigned)txAge);
            reset_unlocked(true);
            hw.unlock();
            hw.sendBreak();
            return;
        }
        IdleAction idleAct = decideIdleWatchdog(
            rxAge, txAge, cfg.idleTimeoutMs);
        if (idleAct == IdleAction::Drop) {
            Log::log().info(
                ALINK_TAG,
                "Idle for %u ms (rxAge=%u txAge=%u, "
                "limit %d) -> dropping link",
                (unsigned)rxAge, (unsigned)rxAge,
                (unsigned)txAge, cfg.idleTimeoutMs);
            reset_unlocked(true);
            hw.unlock();
            hw.sendBreak();
            return;
        }
    }
    if ((now - lastHeartbeatMs_) >=
        (uint32_t)HEARTBEAT_INTERVAL_MS) {
        lastHeartbeatMs_ = now;
        sendFrame_unlocked(PING_CMD);
        Log::log().verbose(ALINK_TAG,
                           "heartbeat PING sent");
        heartbeatPingsMissed_++;
        if (heartbeatPingsMissed_ >=
            HEARTBEAT_MISS_LIMIT) {
            Log::log().warning(
                ALINK_TAG,
                "heartbeat: %d PONG_ACKs missed; peer "
                "is silent -> drop + resweep",
                heartbeatPingsMissed_);
            reset_unlocked(true);
            hw.unlock();
            hw.sendBreak();
            return;
        }
    }
    {
        KeepaliveAction kaAct = decideKeepalive(
            now - lastTxMs, cfg.idleTimeoutMs, false);
        if (kaAct == KeepaliveAction::Emit) {
            sendCobsFrame_unlocked(nullptr, 0);
            lastTxMs = now;
        }
    }
    {
        for (int s = 0; s < 256; s++) {
            if (!ackedPending_[s])
                continue;
            uint32_t age = now - sentAtMs_[s];
            ArqAction arqAct =
                decideArqSlot(age, retxCount_[s],
                              ACK_RTO_MS, MAX_RETX);
            if (arqAct == ArqAction::Hold)
                continue;
            if (arqAct == ArqAction::Drop) {
                Log::log().error(
                    ALINK_TAG,
                    "cobsSeq=%u exceeded MAX_RETX=%d "
                    "(lost wire) -> dropping link",
                    (unsigned)s, (int)MAX_RETX);
                reset_unlocked(true);
                hw.unlock();
                hw.sendBreak();
                return;
            }
            retxCount_[s]++;
            sentAtMs_[s] = now;
            Log::log().warning(
                ALINK_TAG,
                "cobsSeq=%u ACK timeout (age=%lu ms, "
                "retx #%d) -> retransmit",
                (unsigned)s, (unsigned long)age,
                retxCount_[s]);
            if (arqRetxCallback_) {
                pendingRetxBase_ = s;
                hasPendingRetx_ = true;
            } else {
                Log::log().warning(
                    ALINK_TAG,
                    "cobsSeq=%u ACK timeout but no "
                    "ARQ facade registered; "
                    "best-effort mode (will drop on "
                    "MAX_RETX)",
                    (unsigned)s);
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
            Log::log().verbose(
                ALINK_TAG,
                "phase1: PING@baud[%d]=%lu cobsSeq=%u",
                spdI,
                (unsigned long)cfg.allowedBauds[spdI],
                (unsigned)txSeq);
            pingSample++;
            hw.startTimer(phase1ArmMs());
            return;
        }
        if (sweepPhase_ == SweepPhase::PHASE2) {
            Log::log().debug(
                ALINK_TAG,
                "phase2: no PONG_ACK at baud[%d]=%lu "
                "within %d ms, advancing",
                spdI,
                (unsigned long)cfg.allowedBauds[spdI],
                dwells_.phase2[spdI]);
            spdI++;
            if (spdI >= (int)cfg.allowedBaudsCount) {
                Log::log().warning(
                    ALINK_TAG,
                    "phase2: full sweep done, no "
                    "PONG_ACK at any baud; "
                    "falling back to lock at slowest "
                    "baud[%d]=%lu",
                    cfg.allowedBaudsCount - 1,
                    (unsigned long)cfg.allowedBauds
                        [cfg.allowedBaudsCount - 1]);
                sweepPhase_ = SweepPhase::NONE;
                lockOk_unlocked(cfg.allowedBaudsCount -
                                    1,
                                "phase2-fallback");
                return;
            }
            hw.setSpd(cfg.allowedBauds[spdI]);
            sendFrame_unlocked(PING_CMD);
            Log::log().verbose(
                ALINK_TAG,
                "phase2: PING@baud[%d]=%lu cobsSeq=%u",
                spdI,
                (unsigned long)cfg.allowedBauds[spdI],
                (unsigned)txSeq);
            hw.startTimer(dwells_.phase2[spdI]);
            return;
        }
        if (sweepPhase_ == SweepPhase::PHASE3) {
            Log::log().warning(
                ALINK_TAG,
                "phase3: only %d/%d PONG_ACKs at "
                "baud[%d]=%lu; falling back",
                phase3Acks_, PHASE3_ACKS_NEEDED,
                phase3Baud_,
                (unsigned long)
                    cfg.allowedBauds[phase3Baud_]);
            int nextBaud = phase3Baud_ + 1;
            phase3Baud_ = -1;
            phase3Acks_ = 0;
            if (nextBaud >=
                (int)cfg.allowedBaudsCount) {
                int lockedBaud =
                    cfg.allowedBaudsCount - 1;
                sweepPhase_ = SweepPhase::NONE;
                spdI = lockedBaud;
                hw.setSpd(
                    cfg.allowedBauds[lockedBaud]);
                sendFrame_unlocked(PING_CMD);
                Log::log().info(
                    ALINK_TAG,
                    "phase3: locking at slowest "
                    "baud[%d]=%lu",
                    lockedBaud,
                    (unsigned long)
                        cfg.allowedBauds[lockedBaud]);
                lockOk_unlocked(lockedBaud,
                                "phase3-fallback");
                return;
            }
            sweepPhase_ = SweepPhase::PHASE2;
            spdI = nextBaud;
            hw.setSpd(cfg.allowedBauds[spdI]);
            sendFrame_unlocked(PING_CMD);
            hw.startTimer(dwells_.phase2[spdI]);
            return;
        }
        enterPhase1_unlocked();
        return;
    }


    if (emptySweeps == 0 || emptySweeps % 5 == 0) {
        Log::log().info(
            ALINK_TAG,
            "pong SWP: still listening at "
            "baud[%d]=%lu (phase=%d)",
            spdI,
            (unsigned long)cfg.allowedBauds[spdI],
            (int)sweepPhase_);
    }
    emptySweeps++;
    if (emptySweeps > 10) {
        // wasEverOk_ distinguishes a cold-start wiring
        // fault (loud crossover error) from a
        // previously-up link going quiet (calm info
        // log). Never clear wasEverOk_ on reset.
        if (!wasEverOk_) {
            Log::log().error(
                ALINK_TAG,
                "WIRING CHECK (%d ticks no PING): "
                "0 PINGs received at any baud. "
                "Required: Ping TX -> Pong RX  AND  "
                "Pong TX -> Ping RX "
                "(crossover — TX to RX, not TX to "
                "TX). "
                "Also check GND is shared.",
                emptySweeps);
        } else {
            Log::log().info(
                ALINK_TAG,
                "peer silent %d ticks; re-sweeping at "
                "baud[%d]=%lu",
                emptySweeps, spdI,
                (unsigned long)cfg.allowedBauds[spdI]);
        }
        emptySweeps = 5;
    }
    if (sweepPhase_ == SweepPhase::PHASE1) {
        SwpPhaseAction act = decidePongPhase1Timeout();
        if (act == SwpPhaseAction::DropToPhase1) {
            Log::log().debug(
                ALINK_TAG,
                "pong: phase1 dwell expired, "
                "re-arming P1 listen");
            hw.startTimer(phase1ArmMs());
        }
        return;
    }
    if (sweepPhase_ == SweepPhase::PHASE2) {
        int dwell = dwells_.phase2Slave[spdI];
        spdI--;
        if (spdI < 0) {
            Log::log().warning(
                ALINK_TAG,
                "pong: phase2: full sweep done, no "
                "PING at any baud; "
                "dropping to PHASE1");
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
    int maxRetries = (int)cfg.allowedBaudsCount * 2;
    lckRetries++;
    LckAction lckAct =
        decideLckTick(lckRetries, maxRetries);
    if (lckAct == LckAction::SendReq) {
        sendFrame_unlocked(REQ_CMD);
        hw.startTimer(cfg.delayMs);
    } else {
        Log::log().debug(ALINK_TAG,
                         "LCK timeout: no peer reply "
                         "after %d REQs -> re-sweep",
                         lckRetries);
        reset_unlocked(true);
        hw.unlock();
        hw.sendBreak();
    }
}

} // namespace autolink