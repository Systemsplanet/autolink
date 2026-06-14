// ALink.cpp — AutoLink protocol core implementation (v4.0.0).
//
// v4.0.0 changes:
//   * Every COBS frame carries a cobsSeq byte. The receiver uses it to
//     drop stale or out-of-order frames so a wire-byte shift no longer
//     desyncs the message layer (this was the v3.0.0..v3.2.10 bug).
//   * Control frames (PING, REQ, best-ack) are 5 bytes:
//       {0xAA, 0x55, cobsSeq, payload, CRC8(first-4)}
//   * Reliable-mode data frames prepend cobsSeq to the COBS payload:
//       [0x00] [COBS(cobsSeq | payload) | CRC8(cobsSeq | payload)] [0x00]
//   * FIFO length/CRC compare in UtilPing is gone. Echoes are matched by
//     cobsSeq; a gap in cobsSeq means a lost frame, not a desync.
//
// State machine (SWP/LCK/OK), COBS+CRC-8 framing, CRC-16 message layer,
// auto-baud sweep with reliability scoring, idle watchdog, keepalive, and
// error thresholding. All physical I/O goes through the injected ILink so
// this file compiles and runs cleanly on the host for unit testing.
#include "ALink.h"
#include "Log.h"
#include "util/UtilCrc.h"
#include "util/UtilCobs.h"
#include <algorithm>
#include <string.h>

static constexpr const char* ALINK_TAG = "AutoLink";

namespace autolink {

// Scratch buffers must hold one max chunk after COBS + CRC8 + cobsSeq + delimiters.
static_assert(MAX_CHUNK + 6 <= 256, "MAX_CHUNK too large for 256-byte frame buffers");

// Watchdog/keepalive poll interval while in OK: a third of the timeout so a
// keepalive always lands well before the peer's window closes.
int ALink::okTickMs() const {
    int t = cfg.idleTimeoutMs / 3;
    return t < 50 ? 50 : t;
}

const char* StateToStr(State s) {
    switch(s) {
        case State::OK: return "OK";
        case State::SWP: return "SWP";
        case State::LCK: return "LCK";
        default: return "UNK";
    }
}

ALink::ALink(ILink& h, bool isMasterNode, const AutoLinkConfig& config)
    : hw(h), isMaster(isMasterNode), cfg(config),
      state(State::OK), errs(0), spdI(0), pingSample(0), emptySweeps_(0),
      baudSweep((int)config.allowedBauds.size()),
      rxIdx(0), frameRx(*this),
      rxMsgLen(-1), rxMsgCrc(0), lckRetries(0), lastRxMs(0), lastTxMs(0), txBytes(0), rxBytes(0), totalErrs(0), lifetimeErrs(0)
{
    UtilBaudSweep::Config sc;
    sc.pingSamplesPerBaud = config.pingSamplesPerBaud;
    sc.minAcceptRate       = config.minAcceptRate;
    sc.expectedSamples     = -1;  // inherit from pingSamplesPerBaud
    baudSweep.configure(sc);
    hw.bind(this);
    Log::getLog().info(ALINK_TAG, "Initialized as %s. Reliable Mode: %s. cobsSeq tracking: ON",
                       isMaster ? "Ping" : "Pong", cfg.reliableMode ? "ON" : "OFF");
    if (cfg.maxMsg > cfg.streamBufferSize) {
        Log::getLog().error(ALINK_TAG,
            "maxMsg (%u) > streamBufferSize (%u): large messages cannot be reassembled",
            (unsigned)cfg.maxMsg, (unsigned)cfg.streamBufferSize);
    }
}

void ALink::logCobsSeq_unlocked(const char* tag, const char* dir, uint8_t seq) const {
    // INFO so it's visible in the live log by default; not DEBUG, because
    // cobsSeq is the central diagnostic for the v4.0.0 desync fix and we
    // want to see it on the dashboard without having to bump the log level.
    Log::getLog().info(tag, "%s cobsSeq=%u%s",
        dir, (unsigned)seq,
        (lastRxCobsSeqSet_ ? "" : " (first frame on this link)"));
}

void ALink::resetCobsSeq_unlocked() {
    cobsSeq_ = 0;
    lastRxCobsSeqSet_ = false;
    lastRxCobsSeq_ = 0;
    Log::getLog().debug(ALINK_TAG, "cobsSeq reset (sender=%u, lastRx cleared)", (unsigned)cobsSeq_);
}

void ALink::begin() {
    if (!cfg.allowedBauds.empty()) {
        Log::getLog().info(ALINK_TAG, "%s: %d bauds %lu..%lu, %d samples/baud, fastAck=%s",
            isMaster ? "Ping" : "Pong",
            (int)cfg.allowedBauds.size(),
            (unsigned long)cfg.allowedBauds.front(),
            (unsigned long)cfg.allowedBauds.back(),
            cfg.pingSamplesPerBaud,
            cfg.fastBaudLock ? "on" : "off");
    }
    if (isMaster) {
        hw.lock();
        reset_unlocked();
        hw.unlock();
        hw.sendBreak();
    } else {
        hw.lock();
        changeState_unlocked(State::SWP);
        spdI = 0;
        pingSample = 0;
        rxIdx = 0;
        rxMsgLen = -1;
        frameRx.reset();
        baudSweep.resetAll();
        resetCobsSeq_unlocked();
        hw.unlock();
        hw.clearAppBuf();
        hw.setSpd(cfg.allowedBauds[spdI]);
        Log::getLog().info(ALINK_TAG, "SWP Pong testing baud[0]=%lu",
            (unsigned long)cfg.allowedBauds[0]);
        hw.startTimer(cfg.pingSamplesPerBaud * cfg.delayMs);
    }
}

void ALink::changeState_unlocked(State newState) {
    if (state != newState) {
        Log::getLog().info(ALINK_TAG, "State Transition: %s -> %s",
                           StateToStr(state), StateToStr(newState));
        state = newState;
    }
}

int ALink::bestSpd_unlocked() const {
    int best = baudSweep.pickBest();
    if (best < 0) return 0;
    for (int j = 0; j < best; j++) {
        if (baudSweep.scoreAt(j) > 0) return j;
    }
    return best;
}

// v4.0.0: build a 5-byte control frame {0xAA, 0x55, cobsSeq, payload, CRC8(first 4)}.
// The cobsSeq is monotonic per-link and resets only on dropLink_unlocked().
void ALink::sendFrame(uint8_t payload) {
    uint8_t frame[CTRL_FRAME_SIZE];
    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[CTRL_FRAME_SEQ_IDX]     = cobsSeq_;
    frame[CTRL_FRAME_PAYLOAD_IDX] = payload;
    frame[CTRL_FRAME_CRC_IDX]     = UtilCrc::crc8(frame, CTRL_FRAME_SIZE - 1);
    hw.lock();
    if (hw.tx(frame, CTRL_FRAME_SIZE) != CTRL_FRAME_SIZE) {
        Log::getLog().error(ALINK_TAG,
            "sendFrame TX truncated (cobsSeq=%u payload=0x%02X)",
            (unsigned)cobsSeq_, (unsigned)payload);
    }
    hw.flushTx();
    // Don't bump cobsSeq_ for control frames: Pong replies with the same
    // cobsSeq so Ping can match request/ack. Only the *reliable-mode data
    // path* (sendCobsFrame) consumes cobsSeq numbers, one per data frame.
    hw.unlock();
}

void ALink::sendFrame_unlocked(uint8_t payload) {
    uint8_t frame[CTRL_FRAME_SIZE];
    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[CTRL_FRAME_SEQ_IDX]     = cobsSeq_;
    frame[CTRL_FRAME_PAYLOAD_IDX] = payload;
    frame[CTRL_FRAME_CRC_IDX]     = UtilCrc::crc8(frame, CTRL_FRAME_SIZE - 1);
    if (hw.tx(frame, CTRL_FRAME_SIZE) != CTRL_FRAME_SIZE) {
        Log::getLog().error(ALINK_TAG,
            "sendFrame_unlocked TX truncated (cobsSeq=%u payload=0x%02X)",
            (unsigned)cobsSeq_, (unsigned)payload);
    }
    hw.flushTx();
}

// v4.0.0: reliable-mode data frame. Wire format:
//   [0x00] [COBS(cobsSeq | payload) | CRC8(cobsSeq | payload)] [0x00]
// Each data frame consumes one cobsSeq number.
void ALink::sendCobsFrame_unlocked(const uint8_t* b, int n) {
    // Build unencoded: cobsSeq || payload || crc8(cobsSeq || payload)
    uint8_t unenc[MAX_CHUNK + 3];
    unenc[0] = cobsSeq_;
    if (n > 0) memcpy(unenc + 1, b, n);
    unenc[1 + n] = UtilCrc::crc8(unenc, 1 + n);
    size_t rawLen = (size_t)(1 + n) + 1;   // cobsSeq + payload + crc

    // COBS-encode, then bracket with 0x00 delimiters
    uint8_t frame[MAX_CHUNK + 6];
    frame[0] = 0x00;
    size_t encLen = UtilCobs::encode(unenc, rawLen, frame + 1);
    frame[1 + encLen] = 0x00;
    size_t frameLen = encLen + 2;

    int sent = hw.tx(frame, (int)frameLen);
    if ((int)frameLen != sent) {
        Log::getLog().error(ALINK_TAG,
            "sendCobsFrame TX truncated (cobsSeq=%u len=%d accepted=%d)",
            (unsigned)cobsSeq_, (int)frameLen, sent);
    } else {
        Log::getLog().debug(ALINK_TAG, "TX cobsSeq=%u  %d payload bytes  %d wire bytes",
            (unsigned)cobsSeq_, n, (int)frameLen);
    }
    // Bump cobsSeq_ for the next data frame (wrap at 256).
    cobsSeq_ = (uint8_t)(cobsSeq_ + 1);
}

void ALink::sendCobsFrame(const uint8_t* b, int n) {
    hw.lock();
    sendCobsFrame_unlocked(b, n);
    hw.unlock();
}

void ALink::err() {
    hw.lock();
    bool trigger = err_unlocked();
    hw.unlock();
    if (trigger) hw.sendBreak();
}

bool ALink::err_unlocked() {
    if (state != State::OK) return false;
    errs++;
    lifetimeErrs++;
    if (errs > cfg.errThreshold) {
        Log::getLog().info(ALINK_TAG, "Error threshold exceeded (%d > %d). Dropping link.",
                           errs, cfg.errThreshold);
        dropLink_unlocked();
        return true;
    }
    return false;
}

void ALink::clearErr() {
    hw.lock();
    if (errs > 0) errs = 0;
    hw.unlock();
}

int ALink::available() const { return hw.appBufAvailable(); }
int ALink::peek() { return hw.peekAppBuf(); }

int ALink::read() {
    uint8_t b;
    return hw.popAppBuf(&b, 1) == 1 ? b : -1;
}

int ALink::read(uint8_t* b, int max_len) { return hw.popAppBuf(b, max_len); }

int ALink::readStream(uint8_t* b, int n) {
    int got = 0;
    while (got < n) {
        int r = hw.popAppBuf(b + got, n - got);
        if (r <= 0) break;
        got += r;
    }
    return got;
}

int ALink::write(const uint8_t* b, int len) {
    if (len <= 0) return 0;

    if (!cfg.reliableMode) {
        hw.lock();
        bool ok = (state == State::OK);
        if (ok) {
            int sent = hw.tx(b, len);
            if (sent != len) {
                Log::getLog().error(ALINK_TAG,
                    "TX truncated (raw mode): wanted %d bytes, UART accepted %d — dropping link",
                    len, sent);
                err_unlocked();
            } else {
                txBytes += len; lastTxMs = hw.nowMs();
            }
        }
        hw.unlock();
        return ok ? len : 0;
    }

    int offset = 0;
    while (offset < len) {
        int chunk = std::min(len - offset, MAX_CHUNK);
        hw.lock();
        if (state != State::OK) { hw.unlock(); break; }
        sendCobsFrame_unlocked(b + offset, chunk);
        txBytes += chunk;
        lastTxMs = hw.nowMs();
        hw.unlock();
        offset += chunk;
    }
    return offset;
}

int ALink::writeLocked(const uint8_t* b, int len) {
    if (len <= 0) return 0;
    if (state != State::OK) return 0;

    if (!cfg.reliableMode) {
        int sent = hw.tx(b, len);
        if (sent != len) {
            Log::getLog().error(ALINK_TAG,
                "TX truncated (raw locked): wanted %d, UART accepted %d", len, sent);
            err_unlocked();
            return sent;
        }
        txBytes += len; lastTxMs = hw.nowMs();
        return len;
    }

    int offset = 0;
    while (offset < len) {
        if (state != State::OK) break;
        int chunk = std::min(len - offset, MAX_CHUNK);
        sendCobsFrame_unlocked(b + offset, chunk);
        txBytes += chunk; lastTxMs = hw.nowMs();
        offset += chunk;
    }
    return offset;
}

void ALink::dropLink() {
    hw.lock();
    dropLink_unlocked();
    bool needBreak = (state == State::SWP);
    hw.unlock();
    if (needBreak) hw.sendBreak();
}

void ALink::flush() { hw.flushTx(); }

void ALink::flushRx() {
    hw.lock();
    int app_bytes = hw.appBufAvailable();
    hw.clearAppBuf();
    rxMsgLen = -1;
    // v4.0.0: also reset cobsSeq receiver state so the next byte is treated
    // as the start of a fresh sequence (handles the Pong->Ping link-up case
    // where Pong might have stale bytes in its ring from a previous session).
    lastRxCobsSeqSet_ = false;
    lastRxCobsSeq_ = 0;
    hw.unlock();
    hw.flushRxHw();
    Log::getLog().debug(ALINK_TAG,
        "flushRx: cleared %d stream bytes + hw ring (rxMsgLen + cobsSeq rx state reset)",
        app_bytes);
}

bool ALink::sendMsg(const uint8_t* b, int len) {
    if (len <= 0 || (size_t)len > cfg.maxMsg) return false;

    uint16_t c = UtilCrc::crc16(b, len);
    uint8_t hdr[MSG_HDR] = {
        (uint8_t)(len), (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24),
        (uint8_t)(c), (uint8_t)(c >> 8)
    };
    hw.lock();
    if (state != State::OK) { hw.unlock(); return false; }
    if (writeLocked(hdr, MSG_HDR) != MSG_HDR) { hw.unlock(); return false; }
    bool ok = (writeLocked(b, len) == len);
    hw.unlock();
    return ok;
}

int ALink::recvMsg(uint8_t* out, int max_len) {
    hw.lock();

    if (rxMsgLen < 0) {
        if (hw.appBufAvailable() < MSG_HDR) { hw.unlock(); return 0; }
        uint8_t h[MSG_HDR];
        readStream(h, MSG_HDR);
        uint32_t L = (uint32_t)h[0] | ((uint32_t)h[1] << 8) |
                     ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
        rxMsgCrc = (uint16_t)h[4] | ((uint16_t)h[5] << 8);
        if (L == 0 || L > cfg.maxMsg) {
            hw.clearAppBuf();
            rxMsgLen = -1;
            hw.unlock();
            err();
            return -1;
        }
        rxMsgLen = (int)L;
    }

    if (hw.appBufAvailable() < rxMsgLen) { hw.unlock(); return 0; }

    int len = rxMsgLen;
    uint16_t expectedCrc = rxMsgCrc;
    rxMsgLen = -1;

    if (len > max_len) {
        uint8_t sink[256];
        int left = len;
        while (left > 0) left -= readStream(sink, std::min(left, (int)sizeof(sink)));
        hw.unlock();
        err();
        return -1;
    }

    readStream(out, len);
    bool ok = (UtilCrc::crc16(out, len) == expectedCrc);
    hw.unlock();
    if (!ok) { err(); return -1; }
    return len;
}

void ALink::getStats(uint64_t& tx, uint64_t& rx) const {
    hw.lock();
    tx = txBytes;
    rx = rxBytes;
    hw.unlock();
}
void ALink::getStats(uint64_t& tx, uint64_t& rx, uint64_t& errors) const {
    hw.lock();
    tx = txBytes;
    rx = rxBytes;
    errors = totalErrs;
    hw.unlock();
}
void ALink::resetStats() {
    hw.lock(); txBytes = 0; rxBytes = 0; hw.unlock();
}
void ALink::resetErrors() {
    hw.lock(); totalErrs = 0; lifetimeErrs = 0; hw.unlock();
}

State ALink::getState() const { hw.lock(); State s = state; hw.unlock(); return s; }
int ALink::getErrCount() const { hw.lock(); int e = errs; hw.unlock(); return e; }
uint64_t ALink::getLifetimeErrors() const { hw.lock(); uint64_t e = lifetimeErrs; hw.unlock(); return e; }
int ALink::getCurrentSpdIndex() const { hw.lock(); int idx = spdI; hw.unlock(); return idx; }
uint32_t ALink::getCurrentBaud() const {
    hw.lock();
    uint32_t b = (spdI >= 0 && spdI < (int)cfg.allowedBauds.size())
                 ? cfg.allowedBauds[spdI] : 0;
    hw.unlock();
    return b;
}

// v4.0.0: control-frame handler. Same logic as v3.x but reads 5 bytes
// (sync, sync, cobsSeq, payload, crc8) instead of 4. The cobsSeq is
// carried alongside the payload; SWP/LCK command dispatch is unchanged.
void ALink::onRx(const uint8_t* data, int len) {
    hw.lock();
    int i = 0;
    if (state == State::SWP) swpRxBytes_ += len;
    lastRxMs = hw.nowMs();
    bool needSendBreak = false;
    while (i < len) {
        State cur_state = state;

        if (cur_state == State::OK) {
            if (cfg.reliableMode) {
                i += frameRx.feed(data + i, len - i);
                if (state != State::OK) needSendBreak = true;
            } else {
                int n = len - i;
                int acc = hw.pushAppBuf(data + i, n);
                rxBytes += acc;
                if (acc < n) {
                    if (err_unlocked()) needSendBreak = true;
                } else if (errs > 0) {
                    errs = 0;
                }
                i = len;
            }
        }
        else {
            uint8_t b = data[i++];
            if (rxIdx == 0 && b != 0xAA) continue;
            if (rxIdx == 1 && b != 0x55) { rxIdx = 0; continue; }
            rxBuf[rxIdx++] = b;

            if (rxIdx == CTRL_FRAME_SIZE) {
                rxIdx = 0;
                if (UtilCrc::crc8(rxBuf, CTRL_FRAME_SIZE - 1) != rxBuf[CTRL_FRAME_CRC_IDX]) {
                    // Bad CRC on a control frame: corrupt wire, count it.
                    Log::getLog().debug(ALINK_TAG,
                        "control frame bad CRC: %02X %02X %02X %02X %02X",
                        rxBuf[0], rxBuf[1], rxBuf[2], rxBuf[3], rxBuf[4]);
                    if (err_unlocked()) needSendBreak = true;
                    continue;
                }
                uint8_t cobsSeq = rxBuf[CTRL_FRAME_SEQ_IDX];
                uint8_t payload = rxBuf[CTRL_FRAME_PAYLOAD_IDX];

                if (cur_state == State::SWP) {
                    if (!isMaster && payload == REQ_CMD) {
                        int best = bestSpd_unlocked();
                        sendFrame_unlocked(best);
                        hw.setSpd(cfg.allowedBauds[best]);
                        spdI = best;
                        errs = 0;
                        lastRxMs = lastTxMs = hw.nowMs();
                        changeState_unlocked(State::OK);
                        // v4.0.0: entering OK from SWP via REQ — keep the
                        // cobsSeq numbers flowing. lastRxCobsSeqSet_ stays
                        // false until the first data frame arrives, so any
                        // first-byte data from Pong is treated as the start
                        // of a fresh sequence.
                        if (cfg.idleTimeoutMs > 0) hw.startTimer(okTickMs());
                    } else if (isMaster && cfg.fastBaudLock
                               && payload != PING_CMD && payload != REQ_CMD
                               && payload < (int)cfg.allowedBauds.size()) {
                        hw.setSpd(cfg.allowedBauds[payload]);
                        spdI = (int)payload;
                        errs = 0;
                        lastRxMs = lastTxMs = hw.nowMs();
                        Log::getLog().info(ALINK_TAG, "Locked at %lu baud (fast-ack cobsSeq=%u)",
                                           (unsigned long)cfg.allowedBauds[payload],
                                           (unsigned)cobsSeq);
                        changeState_unlocked(State::OK);
                        if (cfg.idleTimeoutMs > 0) hw.startTimer(okTickMs());
                    } else if (payload == PING_CMD && spdI < (int)cfg.allowedBauds.size()) {
                        baudSweep.score(spdI);
                        if (cfg.fastBaudLock && baudSweep.scoreAt(spdI) >= baudSweep.minHitsForReliable()) {
                            int best = baudSweep.pickBest();
                            if (best < 0) best = spdI;
                            for (int j = 0; j < best; j++) {
                                if (baudSweep.scoreAt(j) > 0) { best = j; break; }
                            }
                            sendFrame_unlocked((uint8_t)best);
                            hw.setSpd(cfg.allowedBauds[best]);
                            spdI = best;
                            errs = 0;
                            lastRxMs = lastTxMs = hw.nowMs();
                            Log::getLog().info(ALINK_TAG, "Locked at %lu baud (fast-ack cobsSeq=%u)",
                                               (unsigned long)cfg.allowedBauds[best],
                                               (unsigned)cobsSeq);
                            changeState_unlocked(State::OK);
                            if (cfg.idleTimeoutMs > 0) hw.startTimer(okTickMs());
                        } else {
                            int samples = baudSweep.samplesPerBaud();
                            if (samples < 1) samples = 1;
                            if (baudSweep.scoreAt(spdI) == 1) {
                                Log::getLog().info(ALINK_TAG,
                                    "SWP Pong: first PING at baud[%d]=%lu (cobsSeq=%u)",
                                    spdI, (unsigned long)cfg.allowedBauds[spdI],
                                    (unsigned)cobsSeq);
                            }
                            if (++pingSample >= samples) {
                                pingSample = 0;
                                spdI++;
                                int next = (spdI < (int)cfg.allowedBauds.size()) ? spdI : 0;
                                hw.setSpd(cfg.allowedBauds[next]);
                                hw.startTimer(cfg.pingSamplesPerBaud * cfg.delayMs);
                            }
                        }
                    }
                }
                else if (cur_state == State::LCK) {
                    if (isMaster) {
                        if (payload < (int)cfg.allowedBauds.size()) {
                            hw.setSpd(cfg.allowedBauds[payload]);
                            spdI = (int)payload;
                            errs = 0;
                            lastRxMs = lastTxMs = hw.nowMs();
                            Log::getLog().info(ALINK_TAG, "Locked at %lu baud (cobsSeq=%u)",
                                               (unsigned long)cfg.allowedBauds[payload],
                                               (unsigned)cobsSeq);
                            changeState_unlocked(State::OK);
                            if (cfg.idleTimeoutMs > 0) hw.startTimer(okTickMs());
                        }
                    } else {
                        if (payload == REQ_CMD) {
                            int best = bestSpd_unlocked();
                            sendFrame_unlocked(best);
                            hw.setSpd(cfg.allowedBauds[best]);
                            spdI = best;
                            errs = 0;
                            lastRxMs = lastTxMs = hw.nowMs();
                            Log::getLog().info(ALINK_TAG, "Locked at %lu baud (cobsSeq=%u)",
                                               (unsigned long)cfg.allowedBauds[best],
                                               (unsigned)cobsSeq);
                            changeState_unlocked(State::OK);
                            if (cfg.idleTimeoutMs > 0) hw.startTimer(okTickMs());
                        }
                    }
                }
            }
        }
    }
    hw.unlock();
    if (needSendBreak) hw.sendBreak();
}

void ALink::dropLink_unlocked() {
    if (state == State::OK) totalErrs++;
    changeState_unlocked(State::SWP);
    spdI = 0;
    pingSample = 0;
    rxIdx = 0;
    rxMsgLen = -1;
    frameRx.reset();
    baudSweep.resetAll();
    errs = 0;
    lckRetries = 0;
    emptySweeps_ = 0;
    swpRxBytes_ = 0;
    lastRxMs = hw.nowMs();
    // v4.0.0: cobsSeq sender + receiver reset on every drop. This is the
    // single most important reason a v4.0.0 link survives a desync that
    // would have wedged v3.x: after re-sweep, both sides restart from
    // cobsSeq=0, so any stale bytes from the previous session are
    // immediately rejected as gaps.
    resetCobsSeq_unlocked();
    hw.clearAppBuf();
    hw.setSpd(cfg.allowedBauds[spdI]);
    if (isMaster) hw.startTimer(cfg.delayMs);
    else          hw.startTimer(cfg.pingSamplesPerBaud * cfg.delayMs);
}

void ALink::reset_unlocked() {
    changeState_unlocked(State::SWP);
    spdI = 0;
    rxIdx = 0;
    rxMsgLen = -1;
    frameRx.reset();
    baudSweep.resetAll();
    errs = 0;
    lckRetries = 0;
    emptySweeps_ = 0;
    swpRxBytes_ = 0;
    lastRxMs = hw.nowMs();
    resetCobsSeq_unlocked();
    hw.clearAppBuf();
    hw.setSpd(cfg.allowedBauds[spdI]);
    if (isMaster) hw.startTimer(cfg.delayMs);
    else          hw.startTimer(cfg.pingSamplesPerBaud * cfg.delayMs);
}

// v4.0.0: UtilFrameRx::Listener callback for validated reliable-mode frames.
// cobsSeq comes from the first decoded byte. We do gap/stale detection
// BEFORE pushing payload bytes to the app buffer, so a wire-byte shift
// never reaches the message layer at all.
bool ALink::onPayload(uint8_t cobsSeq, const uint8_t* b, int n) {
    if (state != State::OK) {
        // Frame received while not in OK (race against a drop in another
        // task). Drop silently; the next event will re-evaluate state.
        Log::getLog().debug(ALINK_TAG,
            "RX cobsSeq=%u  %d payload bytes  DROPPED (state != OK)", (unsigned)cobsSeq, n);
        return false;
    }

    // v4.0.0 gap/stale detection.
    if (lastRxCobsSeqSet_) {
        uint8_t expected = (uint8_t)(lastRxCobsSeq_ + 1);
        if (cobsSeq != expected) {
            // Distinguish "stale duplicate / out-of-window" from "true gap".
            // After wraparound at 256, a duplicate of lastRxCobsSeq_ would
            // be exactly 0 ahead, so a generic gap/stale split based purely
            // on arithmetic is ambiguous. Heuristic: if (cobsSeq - lastRx)
            // is small (1..3) it was almost certainly a lost frame (gap).
            // Anything else is treated as a stale frame.
            int diff = (int)cobsSeq - (int)lastRxCobsSeq_;
            if (diff < 0) diff += 256;
            if (diff > 0 && diff <= 3) {
                cobsGaps_++;
                Log::getLog().info(ALINK_TAG,
                    "RX cobsSeq=%u GAP: expected %u, last good=%u  %d payload bytes DROPPED (link stays OK; pipeline self-heals)",
                    (unsigned)cobsSeq, (unsigned)expected, (unsigned)lastRxCobsSeq_, n);
                // Do NOT advance lastRxCobsSeq_ — the gap is real, the next
                // valid frame is expected+1. (Pong, when it sees a gap, will
                // keep sending; the next valid cobsSeq is exactly the one
                // we expected, so the receiver resyncs in one frame.)
                return false;
            }
            cobsStale_++;
            Log::getLog().info(ALINK_TAG,
                "RX cobsSeq=%u STALE: expected %u, last good=%u  %d payload bytes DROPPED (likely previous-session leftover)",
                (unsigned)cobsSeq, (unsigned)expected, (unsigned)lastRxCobsSeq_, n);
            return false;
        }
    }
    lastRxCobsSeq_ = cobsSeq;
    lastRxCobsSeqSet_ = true;

    int acc = hw.pushAppBuf(b, n);
    rxBytes += acc;
    if (acc < n) {
        // v4.0.1: app-buffer-full is an APP-LAYER back-pressure condition,
        // not a wire error. Do NOT count it toward errs (which would
        // trip errThreshold and drop the link, even though the wire is
        // fine). Log it as a warning so the dashboard can show that the
        // app is falling behind the wire, and drop the frame. The next
        // valid cobsSeq frame will be accepted; cobsSeq gap detection
        // will mark it as a gap (the dropped frame's seq is missing from
        // the stream) so the operator can see the count tick up. The
        // link stays OK -- the protocol guarantees the next valid seq
        // is expected+1, so a single dropped frame is recoverable.
        Log::getLog().info(ALINK_TAG,
            "RX cobsSeq=%u app buffer full: wanted %d, accepted %d  "
            "(app falling behind wire; frame dropped, link stays OK)",
            (unsigned)cobsSeq, n, acc);
        cobsGaps_++;   // count it as a gap so the dashboard shows it
        return false;
    }
    Log::getLog().debug(ALINK_TAG,
        "RX cobsSeq=%u  %d payload bytes  -> app buffer (rxBytes=%llu)",
        (unsigned)cobsSeq, n, (unsigned long long)rxBytes);
    if (errs > 0) errs = 0;
    return false;
}

bool ALink::onFrameError() {
    Log::getLog().debug(ALINK_TAG, "frame error (corrupt COBS / bad CRC / oversize)");
    return err_unlocked();
}

void ALink::onBreak() {
    hw.lock();
    Log::getLog().info(ALINK_TAG, "BREAK received -> re-sweep");
    dropLink_unlocked();
    hw.unlock();
}

void ALink::onTimer() {
    hw.lock();
    State s = state;
    int curSpd = spdI;

    if (s == State::OK) {
        if (cfg.idleTimeoutMs > 0) {
            uint32_t now = hw.nowMs();
            if ((uint32_t)(now - lastRxMs) > (uint32_t)cfg.idleTimeoutMs) {
                Log::getLog().info(ALINK_TAG, "Idle for %u ms (limit %d) -> dropping link",
                                   (unsigned)(now - lastRxMs), cfg.idleTimeoutMs);
                dropLink_unlocked();
                hw.unlock();
                hw.sendBreak();
                return;
            }
            if (cfg.reliableMode && (uint32_t)(now - lastTxMs) >= (uint32_t)okTickMs()) {
                // v4.0.0: keepalive is a cobsSeq-bearing 0-payload data
                // frame so the receiver's gap detection sees a continuous
                // stream. Wire:
                //   [0x00] [COBS(cobsSeq | CRC8(cobsSeq))] [0x00]
                //   = [0x00, 0x01, 0x02, CRC8(cobsSeq), 0x00]   (5 bytes)
                {
                    uint8_t unenc[2] = { cobsSeq_, UtilCrc::crc8(&cobsSeq_, 1) };
                    uint8_t ka[5];
                    ka[0] = 0x00;
                    size_t encLen = UtilCobs::encode(unenc, 2, ka + 1);
                    ka[1 + encLen] = 0x00;
                    if (hw.tx(ka, (int)(encLen + 2)) != (int)(encLen + 2)) {
                        Log::getLog().error(ALINK_TAG, "keepalive TX truncated");
                    } else {
                        Log::getLog().debug(ALINK_TAG, "TX keepalive cobsSeq=%u",
                            (unsigned)cobsSeq_);
                        cobsSeq_ = (uint8_t)(cobsSeq_ + 1);
                        lastTxMs = now;
                    }
                }
            }
            hw.unlock();
            hw.startTimer(okTickMs());
            return;
        }
        hw.unlock();
        return;
    }
    hw.unlock();

    if (s == State::SWP) {
        if (isMaster && curSpd < (int)cfg.allowedBauds.size()) {
            if (pingSample == 0) {
                Log::getLog().info(ALINK_TAG, "SWP Ping baud[%d]=%lu",
                    curSpd, (unsigned long)cfg.allowedBauds[curSpd]);
            }
            sendFrame(PING_CMD);
            int samples = baudSweep.samplesPerBaud();
            if (samples < 1) samples = 1;
            if (pingSample + 1 >= samples) {
                hw.lock();
                pingSample = 0;
                spdI++;
                curSpd = spdI;
                hw.unlock();

                if (curSpd < (int)cfg.allowedBauds.size()) {
                    hw.setSpd(cfg.allowedBauds[curSpd]);
                    hw.startTimer(cfg.delayMs);
                } else {
                    hw.lock();
                    changeState_unlocked(State::LCK);
                    lckRetries = 0;
                    spdI = 0;
                    hw.unlock();
                    hw.setSpd(cfg.allowedBauds[0]);
                    if (isMaster) hw.startTimer(cfg.delayMs);
                }
            } else {
                hw.lock();
                pingSample++;
                hw.unlock();
                hw.startTimer(cfg.delayMs);
            }
        } else if (!isMaster) {
            hw.lock();
            if (state == State::SWP) {
                int scored = baudSweep.scoreAt(spdI);
                int needed = baudSweep.minHitsForReliable();
                if (scored < needed) {
                    Log::getLog().info(ALINK_TAG,
                        "SWP Pong baud[%d]=%lu scored %d/%d (%d raw bytes rx), advancing",
                        spdI,
                        (unsigned long)(spdI < (int)cfg.allowedBauds.size()
                                        ? cfg.allowedBauds[spdI] : 0),
                        scored, needed, swpRxBytes_);
                    swpRxBytes_ = 0;
                    spdI++;
                    if (spdI >= (int)cfg.allowedBauds.size()) {
                        bool anyPinged = false;
                        for (int i = 0; i < (int)cfg.allowedBauds.size(); i++) {
                            if (baudSweep.scoreAt(i) > 0) { anyPinged = true; break; }
                        }
                        Log::getLog().info(ALINK_TAG,
                            "SWP Pong: full sweep done, restarting from baud[0]=%lu",
                            (unsigned long)cfg.allowedBauds[0]);
                        spdI = 0;
                        baudSweep.resetAll();

                        if (!anyPinged) {
                            emptySweeps_++;
                            if (emptySweeps_ == 1 || emptySweeps_ % 5 == 0) {
                                Log::getLog().error(ALINK_TAG,
                                    "WIRING CHECK (%d empty sweep(s)): "
                                    "0 raw bytes received at any baud. "
                                    "The Ping node's TX is not reaching this RX pin. "
                                    "Required: Ping TX -> Pong RX  AND  Pong TX -> Ping RX "
                                    "(crossover — TX to RX, not TX to TX). "
                                    "Also check GND is shared. "
                                    "FireBeetle ESP32: GPIO16=D11, GPIO17=D10 on the header — "
                                    "connect Ping D10(GPIO17,TX)->Pong D11(GPIO16,RX) "
                                    "and Pong D10(GPIO17,TX)->Ping D11(GPIO16,RX).",
                                    emptySweeps_);
                            }
                        } else {
                            emptySweeps_ = 0;
                        }
                    }
                    pingSample = 0;
                    Log::getLog().info(ALINK_TAG, "SWP Pong testing baud[%d]=%lu",
                        spdI, (unsigned long)cfg.allowedBauds[spdI]);
                    hw.setSpd(cfg.allowedBauds[spdI]);
                }
                hw.unlock();
                hw.startTimer(cfg.pingSamplesPerBaud * cfg.delayMs);
            } else {
                hw.unlock();
            }
        }
    }
    else if (s == State::LCK && isMaster) {
        sendFrame(REQ_CMD);
        hw.lock();
        lckRetries++;
        hw.unlock();
        if (lckRetries > (int)cfg.allowedBauds.size() * 2) {
            Log::getLog().info(ALINK_TAG, "LCK timeout: no peer reply after %d REQs -> re-sweep",
                               lckRetries);
            hw.lock();
            dropLink_unlocked();
            hw.unlock();
            hw.sendBreak();
            return;
        }
        hw.startTimer(cfg.delayMs);
    }
}

} // namespace autolink
