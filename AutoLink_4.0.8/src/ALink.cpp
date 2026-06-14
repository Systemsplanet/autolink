// ALink.cpp — AutoLink protocol core implementation (v4.0.6).
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
// v4.0.6: fix sendMsg() mutex deadlock.
//   * sendMsg() held hw.lock() then called write(), which also called
//     hw.lock() — a non-recursive re-entrant take on a FreeRTOS
//     xSemaphoreCreateMutex(), which deadlocks the calling task
//     indefinitely. The loop() task froze on the first comm_.send()
//     after link-up; the FreeRTOS timer task continued sending
//     keepalives (never touching write()), so the link stayed alive
//     but no messages were ever delivered. Fixed by replacing the two
//     write() calls with direct sendCobsFrame_unlocked() / hw.tx()
//     calls that assume the lock is already held. As a side-effect,
//     header and payload are now sent inside a single locked scope so
//     no keepalive frame can interleave between them.
//
// v4.0.5: remove flushTx() from sendFrame_unlocked() (control frames).
//   * dropLink_unlocked() / reset_unlocked() merged into reset_unlocked(bool count).
//   * sendFrame() / sendFrame_unlocked() / write() / writeLocked() merged.
//   * The hand-rolled keepalive in onTimer replaced with sendCobsFrame_unlocked
//     (n=0 produces the same wire bytes via the real COBS encoder).
//   * onRx / onTimer split into per-state helpers so the SWP-master branch
//     can hold the lock for the whole call instead of churning it.
//   * Five copies of the "enter OK at baud N" block collapsed to lockOk_unlocked.
//   * Private-member naming normalized (no trailing underscore).
//   * 5 getCobs* getters + 2-arg/3-arg getStats + getLifetimeErrors collapsed
//     to getDiag(Diag&) and getStats(Stats&).
//   * Dead helpers (logCobsSeq_unlocked) and unused ILink seams (pushAppBuf
//     single-byte, popAppBuf no-arg) removed.
//   * Magic numbers (gap window, cobsSeq wrap) named.
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
      state(State::OK), errs(0), spdI(0), pingSample(0), emptySweeps(0),
      baudSweep((int)config.allowedBauds.size()),
      rxIdx(0), frameRx(*this),
      rxMsgLen(-1), rxMsgCrc(0), lckRetries(0), lastRxMs(0), lastTxMs(0),
      txBytes(0), rxBytes(0), discCount(0), frameErrs(0)
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

void ALink::resetSeq_unlocked() {
    txSeq = 0;
    rxSeqSet = false;
    rxSeq = 0;
    // lostMsgs is *not* reset here — it is the lifetime wire-loss tally
    // the dashboard's "lost msgs" card shows, and survives drop/re-sweep
    // on purpose. If the user wants to zero it, that's what resetErrors()
    // is for (which currently zeros discCount and frameErrs; if the
    // dashboard exposes a "lostMsgs=0" button, plumb it there).
    Log::getLog().debug(ALINK_TAG, "cobsSeq reset (sender=%u, lastRx cleared)", (unsigned)txSeq);
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
        reset_unlocked(false);  // begin() is a fresh start, not a disconnect
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
        resetSeq_unlocked();
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
        // v4.0.6: demoted from INFO to DEBUG. State transitions fire on
        // every baud-sweep tick (SWP <-> LCK at ~50 ms cadence during
        // negotiation) and on every link drop / re-lock. At INFO this
        // was drowning the live log on the dashboard — a 4-second
        // negotiation emitted ~80 transition lines for what is, from
        // the operator's perspective, a single "link just locked" event.
        // DEBUG is the right home: a user investigating a stuck link
        // re-enables it with Log::setLevel(DEBUG) and gets the full
        // SWP/LCK chatter. INFO keeps only the user-meaningful events
        // (link up/down, threshold trips, RX errors).
        Log::getLog().debug(ALINK_TAG, "State Transition: %s -> %s",
                            StateToStr(state), StateToStr(newState));
        state = newState;
    }
}

// Highest baud index that has at least minHitsForReliable() decodes.
// Returns 0 if no baud has scored (so a fresh caller falls back to the
// lowest baud). Caller holds the lock.
int ALink::bestSpd_unlocked() const {
    int best = baudSweep.pickBest();
    if (best < 0) return 0;
    // Prefer the highest baud that has *any* decodes, falling back to the
    // scoring-based best. Matches the inline loop that used to live in
    // the SWP fast-ack handler.
    for (int j = 0; j < best; j++) {
        if (baudSweep.scoreAt(j) > 0) return j;
    }
    return best;
}

// v4.0.0: build a 5-byte control frame {0xAA, 0x55, cobsSeq, payload, CRC8(first 4)}.
// The cobsSeq is monotonic per-link and resets only on link drop.
void ALink::sendFrame_unlocked(uint8_t payload) {
    uint8_t frame[CTRL_FRAME_SIZE];
    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[CTRL_FRAME_SEQ_IDX]     = txSeq;
    frame[CTRL_FRAME_PAYLOAD_IDX] = payload;
    frame[CTRL_FRAME_CRC_IDX]     = UtilCrc::crc8(frame, CTRL_FRAME_SIZE - 1);
    if (hw.tx(frame, CTRL_FRAME_SIZE) != CTRL_FRAME_SIZE) {
        Log::getLog().error(ALINK_TAG,
            "sendFrame TX truncated (cobsSeq=%u payload=0x%02X)",
            (unsigned)txSeq, (unsigned)payload);
    }
    // v4.0.5: removed hw.flushTx(). v4.0.0..v4.0.4 called
    //   uart_wait_tx_done(uart_num, pdMS_TO_TICKS(100))
    // here, which blocks the calling task until the UART TX FIFO drains
    // to empty. Control frames are 5 bytes — at 115200 baud that's
    // ~0.4 ms of wire time — but the FreeRTOS tick granularity means
    // the task yields and may not be rescheduled for a full tick
    // (1 ms). The data path (sendCobsFrame_unlocked) does NOT call
    // flushTx(), so the data path was unaffected, but the control
    // path (PING/REQ/fast-ack/keepalive) was unnecessarily serialized.
    // The auto-sized TX ring is large enough to accept 5 bytes
    // instantly and non-blocking; hw.tx() returning == CTRL_FRAME_SIZE
    // is the success signal. Users who need an explicit drain still
    // have the public ALink::flush() / AutoLink::flush() entry point.
    // DEBUG log here makes the "no flush" decision visible in the log;
    // in practice this fires on every PING/REQ/fast-ack/keepalive so
    // it's gated behind Log::DEBUG (not INFO) to keep the steady-state
    // log readable.
    Log::getLog().debug(ALINK_TAG,
        "sendFrame TX  cobsSeq=%u  payload=0x%02X  (no flushTx, v4.0.5)",
        (unsigned)txSeq, (unsigned)payload);
    // Don't bump txSeq for control frames: Pong replies with the same
    // cobsSeq so Ping can match request/ack. Only the *reliable-mode data
    // path* (sendCobsFrame_unlocked) consumes cobsSeq numbers, one per
    // data frame.
}

void ALink::sendFrame(uint8_t payload) {
    hw.lock();
    sendFrame_unlocked(payload);
    hw.unlock();
}

// v4.0.0: reliable-mode data frame. Wire format:
//   [0x00] [COBS(cobsSeq | payload) | CRC8(cobsSeq | payload)] [0x00]
// Each data frame consumes one cobsSeq number. With n=0 this is the
// keepalive shape: a cobsSeq-bearing frame so the receiver's gap
// detection sees a continuous stream even when the app has nothing to
// send. Wire is always 5 bytes — see the keepalive comment in onTimer.
void ALink::sendCobsFrame_unlocked(const uint8_t* b, int n) {
    if (n < 0) n = 0;
    if (n > MAX_CHUNK) n = MAX_CHUNK;

    // Build unencoded: cobsSeq || payload || crc8(cobsSeq || payload)
    uint8_t unenc[MAX_CHUNK + 3];
    unenc[0] = txSeq;
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
            (unsigned)txSeq, (int)frameLen, sent);
    } else if (cfg.reliableMode) {
        Log::getLog().debug(ALINK_TAG, "TX cobsSeq=%u  %d payload bytes  %d wire bytes",
            (unsigned)txSeq, n, (int)frameLen);
    }
    // Bump txSeq for the next data frame (wrap at COBS_SEQ_WRAP).
    txSeq = (uint8_t)(txSeq + 1);
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
    frameErrs++;
    if (errs > cfg.errThreshold) {
        Log::getLog().info(ALINK_TAG, "Error threshold exceeded (%d > %d). Dropping link.",
                           errs, cfg.errThreshold);
        reset_unlocked(true);
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
    hw.lock();
    if (state != State::OK) { hw.unlock(); return 0; }

    if (!cfg.reliableMode) {
        int sent = hw.tx(b, len);
        if (sent != len) {
            Log::getLog().error(ALINK_TAG,
                "TX truncated (raw): wanted %d, UART accepted %d — dropping link",
                len, sent);
            err_unlocked();
            hw.unlock();
            return sent;
        }
        txBytes += len; lastTxMs = hw.nowMs();
        hw.unlock();
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
    hw.unlock();
    return offset;
}

void ALink::dropLink() {
    hw.lock();
    reset_unlocked(true);  // canonical "the link went down" path
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
    rxSeqSet = false;
    rxSeq = 0;
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

    // Use unlocked send paths — sendMsg already holds hw.lock().
    // Calling write() here would re-enter the non-recursive mutex and deadlock.
    // Sending header and payload inside one lock scope also guarantees no
    // keepalive frame (from the timer task) can interleave between them.
    bool ok = true;
    if (cfg.reliableMode) {
        sendCobsFrame_unlocked(hdr, MSG_HDR);
        int offset = 0;
        while (offset < len) {
            if (state != State::OK) { ok = false; break; }
            int chunk = std::min(len - offset, MAX_CHUNK);
            sendCobsFrame_unlocked(b + offset, chunk);
            txBytes += chunk;
            lastTxMs = hw.nowMs();
            offset += chunk;
        }
    } else {
        if (hw.tx(hdr, MSG_HDR) != MSG_HDR) {
            ok = false;
        } else {
            int sent = hw.tx(b, len);
            txBytes += sent;
            lastTxMs = hw.nowMs();
            ok = (sent == len);
        }
    }

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

void ALink::getStats(Stats& s) const {
    hw.lock();
    s.tx        = txBytes;
    s.rx        = rxBytes;
    s.discCount = discCount;
    s.frameErrs = frameErrs;
    hw.unlock();
}
void ALink::resetStats() {
    hw.lock(); txBytes = 0; rxBytes = 0; hw.unlock();
}
void ALink::resetErrors() {
    hw.lock(); discCount = 0; frameErrs = 0; hw.unlock();
}

State ALink::getState() const { hw.lock(); State s = state; hw.unlock(); return s; }
int ALink::getErrCount() const { hw.lock(); int e = errs; hw.unlock(); return e; }
int ALink::getCurrentSpdIndex() const { hw.lock(); int idx = spdI; hw.unlock(); return idx; }
uint32_t ALink::getCurrentBaud() const {
    hw.lock();
    uint32_t b = (spdI >= 0 && spdI < (int)cfg.allowedBauds.size())
                 ? cfg.allowedBauds[spdI] : 0;
    hw.unlock();
    return b;
}

void ALink::getDiag(Diag& d) const {
    hw.lock();
    d.txSeq    = txSeq;
    d.rxSeqSet = rxSeqSet;
    d.rxSeq    = rxSeq;
    d.gaps     = gaps;
    d.stale    = stale;
    d.lostMsgs = lostMsgs;
    hw.unlock();
}

// =============================================================================
// lockOk_unlocked — the "enter OK at baud N" block, single source of truth.
//
// All five callsites that used to paste the 6-line sequence
// (setSpd / spdI / errs=0 / lastRx=lastTx / changeState(OK) / startTimer)
// now funnel through here. The one piece of variation is the log label
// ("fast-ack", "REQ") — passed as `tag` so the caller's log line reads
// the way it always did. Caller holds the lock.
// =============================================================================
void ALink::lockOk_unlocked(int idx, const char* tag) {
    hw.setSpd(cfg.allowedBauds[idx]);
    spdI = idx;
    errs = 0;
    lastRxMs = lastTxMs = hw.nowMs();
    Log::getLog().info(ALINK_TAG, "Locked at %lu baud (%s)",
                       (unsigned long)cfg.allowedBauds[idx], tag);
    changeState_unlocked(State::OK);
    if (cfg.idleTimeoutMs > 0) hw.startTimer(okTickMs());
}

// =============================================================================
// onRx — split into OK-path (delegates to frameRx.feed) and control-frame
// path (delegates to ctrlFrameReady_unlocked). The control-frame handlers
// handleSwp_unlocked / handleLck_unlocked keep the lock for the whole
// frame so a state transition + timer arm + reply don't race with another
// RX event.
// =============================================================================
void ALink::onRx(const uint8_t* data, int len) {
    hw.lock();
    int i = 0;
    if (state == State::SWP) swpRxBytes += len;
    lastRxMs = hw.nowMs();
    bool needSendBreak = false;
    while (i < len) {
        State curState = state;

        if (curState == State::OK) {
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
            // Control-frame accumulator. Strips the 0xAA 0x55 preamble
            // and feeds the rest into the 5-byte rxBuf. Caller of
            // ctrlFrameReady_unlocked holds the lock — see that fn.
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
                if (ctrlFrameReady_unlocked(cobsSeq, payload, curState)) {
                    needSendBreak = true;
                }
            }
        }
    }
    hw.unlock();
    if (needSendBreak) hw.sendBreak();
}

bool ALink::ctrlFrameReady_unlocked(uint8_t cobsSeq, uint8_t payload, State curState) {
    if (curState == State::SWP) return handleSwp_unlocked(cobsSeq, payload);
    if (curState == State::LCK) return handleLck_unlocked(cobsSeq, payload);
    return false;  // we shouldn't be called in OK; OK frames go through frameRx.feed
}

bool ALink::handleSwp_unlocked(uint8_t cobsSeq, uint8_t payload) {
    if (!isMaster && payload == REQ_CMD) {
        // Pong saw Ping's REQ at the top of the sweep: pick the best baud,
        // tell Ping, and both sides enter OK at that baud.
        int best = bestSpd_unlocked();
        sendFrame_unlocked((uint8_t)best);
        lockOk_unlocked(best, "REQ");
        return false;
    }

    if (isMaster && cfg.fastBaudLock
        && payload != PING_CMD && payload != REQ_CMD
        && payload < (int)cfg.allowedBauds.size()) {
        // Pong sent a fast-ack with a baud index instead of a PING/REQ.
        // Lock immediately at that baud.
        lockOk_unlocked((int)payload, "fast-ack");
        return false;
    }

    if (payload == PING_CMD && spdI < (int)cfg.allowedBauds.size()) {
        // Pong-side: score this PING and consider locking early (fast-ack).
        baudSweep.score(spdI);
        if (cfg.fastBaudLock && baudSweep.scoreAt(spdI) >= baudSweep.minHitsForReliable()) {
            int best = bestSpd_unlocked();
            sendFrame_unlocked((uint8_t)best);
            lockOk_unlocked(best, "fast-ack");
            return false;
        }
        // Not enough samples yet; advance after the last sample in this
        // window so the next baud is tested next tick.
        int samples = baudSweep.samplesPerBaud();
        if (samples < 1) samples = 1;
        if (baudSweep.scoreAt(spdI) == 1) {
            Log::getLog().info(ALINK_TAG,
                "SWP Pong: first PING at baud[%d]=%lu (cobsSeq=%u)",
                spdI, (unsigned long)cfg.allowedBauds[spdI], (unsigned)cobsSeq);
        }
        if (++pingSample >= samples) {
            pingSample = 0;
            spdI++;
            int next = (spdI < (int)cfg.allowedBauds.size()) ? spdI : 0;
            hw.setSpd(cfg.allowedBauds[next]);
            hw.startTimer(cfg.pingSamplesPerBaud * cfg.delayMs);
        }
        return false;
    }
    return false;
}

bool ALink::handleLck_unlocked(uint8_t cobsSeq, uint8_t payload) {
    (void)cobsSeq;   // LCK uses the payload as a baud index; cobsSeq is just echoed back
    if (isMaster) {
        // Pong's REQ-reply: it carries a baud index. Lock there.
        if (payload < (int)cfg.allowedBauds.size()) {
            lockOk_unlocked((int)payload, "REQ");
        }
        return false;
    }
    // Pong-side in LCK: ignore everything except Ping's REQ.
    if (payload == REQ_CMD) {
        int best = bestSpd_unlocked();
        sendFrame_unlocked((uint8_t)best);
        lockOk_unlocked(best, "REQ");
    }
    return false;
}

// =============================================================================
// dropLink_unlocked / reset_unlocked — merged. The `count` flag picks
// between "this is a disconnect" (counts toward discCount, called from
// err_unlocked, dropLink, onBreak) and "this is a fresh start" (no count,
// called from begin). v4.0.2's reset_unlocked was missing the
// `pingSample = 0` line — a latent inconsistency that disappears on merge.
// =============================================================================
void ALink::reset_unlocked(bool count) {
    if (count && state == State::OK) discCount++;
    changeState_unlocked(State::SWP);
    spdI = 0;
    pingSample = 0;
    rxIdx = 0;
    rxMsgLen = -1;
    frameRx.reset();
    baudSweep.resetAll();
    errs = 0;
    lckRetries = 0;
    emptySweeps = 0;
    swpRxBytes = 0;
    lastRxMs = hw.nowMs();
    // v4.0.0: cobsSeq sender + receiver reset on every drop. This is the
    // single most important reason a v4.0.0 link survives a desync that
    // would have wedged v3.x: after re-sweep, both sides restart from
    // cobsSeq=0, so any stale bytes from the previous session are
    // immediately rejected as gaps.
    resetSeq_unlocked();
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
        // task). Per the UtilFrameRx::Listener contract, return true so
        // feed() stops handing the rest of this event to the parser — the
        // rest of the bytes belong to the stale session, not us.
        Log::getLog().debug(ALINK_TAG,
            "RX cobsSeq=%u  %d payload bytes  DROPPED (state != OK)", (unsigned)cobsSeq, n);
        return true;
    }

    // v4.0.0 gap/stale detection.
    if (rxSeqSet) {
        uint8_t expected = (uint8_t)(rxSeq + 1);
        if (cobsSeq != expected) {
            // Distinguish "stale duplicate / out-of-window" from "true gap".
            // After wraparound at COBS_SEQ_WRAP, a duplicate of rxSeq would
            // be exactly 0 ahead, so a generic gap/stale split based purely
            // on arithmetic is ambiguous. Heuristic: if (cobsSeq - rxSeq)
            // is small (1..MAX_GAP_RESYNC) it was almost certainly a lost
            // frame (gap). Anything else is treated as a stale frame.
            int diff = (int)cobsSeq - (int)rxSeq;
            if (diff < 0) diff += COBS_SEQ_WRAP;
            if (diff > 0 && diff <= MAX_GAP_RESYNC) {
                gaps++;
                // `diff` is the count of cobsSeqs jumped over. When
                // diff==1 we lost exactly 1 message (the expected one);
                // when diff==3 we lost 3 (e.g. expected=2, cobsSeq=5).
                // `lostMsgs` is the lifetime wire-loss tally the dashboard
                // shows, separate from `gaps` so a single burst loss
                // (1 gap event, many missing messages) doesn't look the
                // same as N independent single-message gaps.
                lostMsgs += (uint64_t)(diff - 1);
                Log::getLog().info(ALINK_TAG,
                    "RX cobsSeq=%u GAP: expected %u, last good=%u  %d payload bytes DROPPED (link stays OK; pipeline self-heals)  +%d lost msg%s (total %llu)",
                    (unsigned)cobsSeq, (unsigned)expected, (unsigned)rxSeq, n,
                    (int)(diff - 1), (diff - 1) == 1 ? "" : "s",
                    (unsigned long long)lostMsgs);
                // Do NOT advance rxSeq — the gap is real, the next
                // valid frame is expected+1. (Pong, when it sees a gap, will
                // keep sending; the next valid cobsSeq is exactly the one
                // we expected, so the receiver resyncs in one frame.) Link
                // is OK, so return false — feed() should keep parsing the
                // tail of the current event.
                return false;
            }
            stale++;
            Log::getLog().info(ALINK_TAG,
                "RX cobsSeq=%u STALE: expected %u, last good=%u  %d payload bytes DROPPED (likely previous-session leftover)",
                (unsigned)cobsSeq, (unsigned)expected, (unsigned)rxSeq, n);
            // Link is OK, so return false — feed() should keep parsing the
            // tail of the current event. The stale frame is dropped, not
            // the rest of the RX event.
            return false;
        }
    }
    rxSeq = cobsSeq;
    rxSeqSet = true;

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
        gaps++;   // count it as a gap so the dashboard shows it
        // Return true so feed() stops handing the rest of this event to
        // the parser. The wire is fine but the consumer is wedged; draining
        // the rest of the event would just queue more bytes the consumer
        // can't read. The next UART event will retry once the app drains.
        return true;
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
    reset_unlocked(true);  // a peer-driven BREAK is a real disconnect
    hw.unlock();
}

// =============================================================================
// onTimer — split into OK / SWP / LCK helpers. The OK path is the
// watchdog + keepalive; SWP splits into master (sends PING and steps
// through bauds) and Pong (scores the current baud and advances); LCK
// is the master sending REQs with a retry cap. The SWP-master branch
// in v4.0.2 churned lock/unlock 4 times within one call — that made
// it easy to introduce a state-read race against onRx. v4.0.3 keeps
// the lock for the whole branch.
// =============================================================================
void ALink::onTimer() {
    State s;
    int curSpd;
    hw.lock();
    s = state;
    curSpd = spdI;

    if (s == State::OK)       onTimerOk_unlocked();
    else if (s == State::SWP) onTimerSwp_unlocked();
    else if (s == State::LCK && isMaster) onTimerLck_unlocked();
    else { hw.unlock(); (void)curSpd; return; }
    hw.unlock();
}

void ALink::onTimerOk_unlocked() {
    if (cfg.idleTimeoutMs <= 0) return;
    uint32_t now = hw.nowMs();
    if ((uint32_t)(now - lastRxMs) > (uint32_t)cfg.idleTimeoutMs) {
        Log::getLog().info(ALINK_TAG, "Idle for %u ms (limit %d) -> dropping link",
                           (unsigned)(now - lastRxMs), cfg.idleTimeoutMs);
        reset_unlocked(true);
        hw.unlock();
        hw.sendBreak();
        return;
    }
    if (cfg.reliableMode && (uint32_t)(now - lastTxMs) >= (uint32_t)okTickMs()) {
        // Keepalive shape: a cobsSeq-bearing 0-payload data frame. The
        // 0-payload form of sendCobsFrame_unlocked emits the exact same
        // 5 wire bytes as the hand-rolled encoder we used to inline here
        // (verified: for any cobsSeq in 0..255, the resulting wire is
        // [0x00, COBS(cobsSeq|CRC8(cobsSeq)), 0x00] = 5 bytes). Funneling
        // through the real encoder means the keepalive can never drift
        // from the data path's wire format.
        sendCobsFrame_unlocked(nullptr, 0);
        lastTxMs = now;
    }
    hw.startTimer(okTickMs());
}

void ALink::onTimerSwp_unlocked() {
    if (isMaster) {
        if (spdI >= (int)cfg.allowedBauds.size()) {
            // Past the last baud — should have transitioned to LCK already,
            // but if a re-sweep raced with the end of the baud list, finish
            // the transition now.
            changeState_unlocked(State::LCK);
            lckRetries = 0;
            spdI = 0;
            hw.setSpd(cfg.allowedBauds[0]);
            if (isMaster) hw.startTimer(cfg.delayMs);
            return;
        }
        if (pingSample == 0) {
            // v4.0.6: demoted from INFO to DEBUG. Fires on every baud
            // index transition during the sweep — once per baud at a
            // 50 ms cadence for a 5-baud sweep, so ~10 lines per
            // negotiation plus one per re-sweep. The Pong's matching
            // log "SWP Pong: full sweep done" stays at INFO (one line
            // per sweep, much rarer); the per-baud "Ping is now testing
            // baud[N]" is DEBUG territory.
            Log::getLog().debug(ALINK_TAG, "SWP Ping baud[%d]=%lu",
                spdI, (unsigned long)cfg.allowedBauds[spdI]);
        }
        sendFrame_unlocked(PING_CMD);
        int samples = baudSweep.samplesPerBaud();
        if (samples < 1) samples = 1;
        if (pingSample + 1 >= samples) {
            pingSample = 0;
            spdI++;
            if (spdI < (int)cfg.allowedBauds.size()) {
                hw.setSpd(cfg.allowedBauds[spdI]);
                hw.startTimer(cfg.delayMs);
            } else {
                changeState_unlocked(State::LCK);
                lckRetries = 0;
                spdI = 0;
                hw.setSpd(cfg.allowedBauds[0]);
                hw.startTimer(cfg.delayMs);
            }
        } else {
            pingSample++;
            hw.startTimer(cfg.delayMs);
        }
        return;
    }

    // Pong-side SWP: score the current baud, advance, restart from 0
    // when we wrap, fire the WIRING CHECK if a full sweep heard nothing.
    int scored = baudSweep.scoreAt(spdI);
    int needed = baudSweep.minHitsForReliable();
    if (scored < needed) {
        Log::getLog().info(ALINK_TAG,
            "SWP Pong baud[%d]=%lu scored %d/%d (%d raw bytes rx), advancing",
            spdI,
            (unsigned long)(spdI < (int)cfg.allowedBauds.size()
                            ? cfg.allowedBauds[spdI] : 0),
            scored, needed, swpRxBytes);
        swpRxBytes = 0;
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
                emptySweeps++;
                if (emptySweeps == 1 || emptySweeps % 5 == 0) {
                    Log::getLog().error(ALINK_TAG,
                        "WIRING CHECK (%d empty sweep(s)): "
                        "0 raw bytes received at any baud. "
                        "The Ping node's TX is not reaching this RX pin. "
                        "Required: Ping TX -> Pong RX  AND  Pong TX -> Ping RX "
                        "(crossover — TX to RX, not TX to TX). "
                        "Also check GND is shared. "
                        "Check the rxPin/txPin you passed to the AutoLink "
                        "constructor match your board's pinout.",
                        emptySweeps);
                }
            } else {
                emptySweeps = 0;
            }
        }
        pingSample = 0;
        Log::getLog().info(ALINK_TAG, "SWP Pong testing baud[%d]=%lu",
            spdI, (unsigned long)cfg.allowedBauds[spdI]);
        hw.setSpd(cfg.allowedBauds[spdI]);
    }
    hw.startTimer(cfg.pingSamplesPerBaud * cfg.delayMs);
}

void ALink::onTimerLck_unlocked() {
    sendFrame_unlocked(REQ_CMD);
    lckRetries++;
    if (lckRetries > (int)cfg.allowedBauds.size() * 2) {
        // v4.0.6: demoted from INFO to DEBUG. Fires whenever the master
        // gives up on a sweep and forces a re-sweep. From the operator's
        // perspective, the visible signal that a re-sweep happened is
        // the "Locked at N baud" line that immediately precedes this
        // one, which is already at INFO. The detailed "how many REQs
        // did we try before giving up" is DEBUG.
        Log::getLog().debug(ALINK_TAG, "LCK timeout: no peer reply after %d REQs -> re-sweep",
                            lckRetries);
        reset_unlocked(true);
        hw.unlock();
        hw.sendBreak();
        return;
    }
    hw.startTimer(cfg.delayMs);
}

} // namespace autolink
