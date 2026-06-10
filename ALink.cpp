#include "ALink.h"
#include "Log.h"
#include "UtilCrc.h"
#include "UtilCobs.h"
#include <algorithm>
#include <string.h>

static constexpr const char* ALINK_TAG = "AutoLink";

namespace autolink {

// Scratch buffers must hold one max chunk after COBS + CRC8 + delimiters.
static_assert(MAX_CHUNK + 5 <= 256, "MAX_CHUNK too large for 256-byte frame buffers");

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
      state(State::OK), errs(0), spdI(0), rxIdx(0), frameRx(*this),
      rxMsgLen(-1), rxMsgCrc(0), lckRetries(0), lastRxMs(0), lastTxMs(0), txBytes(0), rxBytes(0), totalErrs(0)
{
    scores.resize(cfg.allowedBauds.size(), 0);
    hw.bind(this);
    Log::getLog().info(ALINK_TAG, "Initialized as %s. Reliable Mode: %s",
                       isMaster ? "Master" : "Slave", cfg.reliableMode ? "ON" : "OFF");
    if (cfg.maxMsg > cfg.streamBufferSize) {
        Log::getLog().error(ALINK_TAG,
            "maxMsg (%u) > streamBufferSize (%u): large messages cannot be reassembled",
            (unsigned)cfg.maxMsg, (unsigned)cfg.streamBufferSize);
    }
}

void ALink::begin() {
    if (isMaster) {
        // Run the local drop explicitly: a sender never receives its own BREAK.
        // First init -- don't count this as an error. A real "peer went away"
        // comes through the public onBreak() path later and counts.
        hw.lock();
        dropLink_unlocked(false);
        hw.unlock();
        hw.sendBreak();
    } else {
        hw.lock();
        changeState_unlocked(State::SWP);
        spdI = 0; rxIdx = 0;
        rxMsgLen = -1;
        frameRx.reset();
        for (int i = 0; i < (int)scores.size(); i++) scores[i] = 0;
        hw.unlock();
        hw.clearAppBuf();
        hw.setSpd(cfg.allowedBauds[0]);
        // Slave does not run the sweep clock; the master drives it.
    }
}

void ALink::changeState_unlocked(State newState) {
    if (state != newState) {
        Log::getLog().info(ALINK_TAG, "State Transition: %s -> %s",
                           StateToStr(state), StateToStr(newState));
        state = newState;
    }
}

// Pick the fastest baud (highest index) the slave actually decoded a PING on.
// Falls back to index 0 if nothing scored. Caller must hold the lock.
int ALink::bestSpd_unlocked() const {
    int best = 0;
    for (int j = 1; j < (int)cfg.allowedBauds.size(); j++) {
        if (scores[j] > 0) best = j;
    }
    return best;
}

void ALink::sendFrame(uint8_t payload) {
    uint8_t frame[4] = {0xAA, 0x55, payload, 0};
    frame[3] = UtilCrc::crc8(frame, 3);
    hw.lock();
    hw.tx(frame, 4);
    hw.flushTx();
    hw.unlock();
}

// Caller must already hold the lock. Used from the locked onRx path.
void ALink::sendFrame_unlocked(uint8_t payload) {
    uint8_t frame[4] = {0xAA, 0x55, payload, 0};
    frame[3] = UtilCrc::crc8(frame, 3);
    hw.tx(frame, 4);
    hw.flushTx();
}

void ALink::err() {
    hw.lock();
    bool trigger = err_unlocked();
    hw.unlock();
    if (trigger) hw.sendBreak();
}

// Caller must hold the lock. Returns whether the threshold was tripped; on
// trip the local link is dropped here (caller sends the BREAK to the peer).
//
// Two distinct counters are kept:
//   - `errs` (the threshold window) only ticks while in OK. The link is
//     supposed to be quiet during SWP/LCK, so thresholding only fires when
//     the link was actually up.
//   - `totalErrs` (the lifetime cumulative count) ticks on EVERY entry --
//     public err() callers and direct err_unlocked() callers (the frame
//     parser paths in onRx/onPayload/onFrameError). Bumping it first means
//     a bad frame arriving during SWP/LCK is still visible to the user.
//     Without this, a cable bounce that recovers cleanly shows err=0
//     forever -- the very case the counter was added to expose.
bool ALink::err_unlocked() {
    totalErrs++;
    if (state != State::OK) return false;
    errs++;
    if (errs > cfg.errThreshold) {
        Log::getLog().info(ALINK_TAG, "Error threshold exceeded (%d > %d). Dropping link.",
                           errs, cfg.errThreshold);
        dropLink_unlocked(false);   // local SWP + retune; we still hold the lock
                                    // (totalErrs was already bumped at entry)
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
        if (ok) { hw.tx(b, len); txBytes += len; lastTxMs = hw.nowMs(); }
        hw.unlock();
        return ok ? len : 0;
    }

    int offset = 0;
    uint8_t unenc[MAX_CHUNK + 1];
    uint8_t frame[MAX_CHUNK + 5];
    while(offset < len) {
        // Build the whole frame outside the lock; only the actual tx and the
        // state read need it. Encoding is pure pointer math, so it's fine to
        // run unlocked.
        int chunk = std::min(len - offset, MAX_CHUNK);
        memcpy(unenc, b + offset, chunk);
        unenc[chunk] = UtilCrc::crc8(unenc, chunk);

        frame[0] = 0x00;
        size_t encLen = UtilCobs::encode(unenc, chunk + 1, frame + 1);
        frame[1 + encLen] = 0x00;

        hw.lock();
        if (state != State::OK) { hw.unlock(); break; }
        // Hold the lock across the whole frame and the stats bump so a
        // concurrent reader (onRx) sees a consistent state, and so two
        // writers can't tear the per-frame byte sequence. Each frame is
        // <= 255 B, well under the TX FIFO. Note: uart_write_bytes copies
        // into the driver ring and returns quickly; flushTx() (called by
        // the app) is what blocks until the physical TX drains.
        hw.tx(frame, encLen + 2);
        txBytes += chunk;
        lastTxMs = hw.nowMs();
        hw.unlock();

        offset += chunk;
    }
    return offset;
}

// Variant for callers that already hold the lock (e.g. sendMsg, which must
// keep the lock across header + payload to avoid a window where the link
// could drop between them). Returns bytes accepted; stops on state change.
int ALink::writeLocked(const uint8_t* b, int len) {
    if (len <= 0) return 0;
    if (state != State::OK) return 0;

    if (!cfg.reliableMode) {
        hw.tx(b, len);
        txBytes += len;
        lastTxMs = hw.nowMs();
        return len;
    }

    int offset = 0;
    uint8_t unenc[MAX_CHUNK + 1];
    uint8_t frame[MAX_CHUNK + 5];
    while (offset < len) {
        if (state != State::OK) break;
        int chunk = std::min(len - offset, MAX_CHUNK);
        memcpy(unenc, b + offset, chunk);
        unenc[chunk] = UtilCrc::crc8(unenc, chunk);
        frame[0] = 0x00;
        size_t encLen = UtilCobs::encode(unenc, chunk + 1, frame + 1);
        frame[1 + encLen] = 0x00;
        hw.tx(frame, encLen + 2);
        txBytes += chunk;
        lastTxMs = hw.nowMs();
        offset += chunk;
    }
    return offset;
}

void ALink::flush() { hw.flushTx(); }

bool ALink::sendMsg(const uint8_t* b, int len) {
    if (len <= 0 || (size_t)len > cfg.maxMsg) return false;

    uint16_t c = UtilCrc::crc16(b, len);
    uint8_t hdr[MSG_HDR] = {
        (uint8_t)(len), (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24),
        (uint8_t)(c), (uint8_t)(c >> 8)
    };
    // Hold the lock for header + payload as a single atomic transmission. Two
    // separate write() calls would leave a window where the link could drop
    // between header and payload, leaving the receiver stranded waiting for
    // bytes that will never come.
    hw.lock();
    if (state != State::OK) { hw.unlock(); return false; }
    if (writeLocked(hdr, MSG_HDR) != MSG_HDR) { hw.unlock(); return false; }
    bool ok = (writeLocked(b, len) == len);
    hw.unlock();
    return ok;
}

int ALink::recvMsg(uint8_t* out, int max_len) {
    // Hold the lock for the whole reassembly sequence: the UART task can be
    // concurrently pushing bytes into the app buffer and the onRx reliable
    // path holds the same lock, so a length/CRC snapshot, a read, and the
    // state reset are all observed atomically.
    hw.lock();

    if (rxMsgLen < 0) {
        if (hw.appBufAvailable() < MSG_HDR) { hw.unlock(); return 0; }
        uint8_t h[MSG_HDR];
        readStream(h, MSG_HDR);
        uint32_t L = (uint32_t)h[0] | ((uint32_t)h[1] << 8) |
                     ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
        rxMsgCrc = (uint16_t)h[4] | ((uint16_t)h[5] << 8);
        if (L == 0 || L > cfg.maxMsg) {
            // Garbage length: stream is desynced. Flush and signal an error so
            // the link can drop and re-negotiate. err() takes the lock itself,
            // so release first.
            hw.clearAppBuf();
            rxMsgLen = -1;
            hw.unlock();
            err();
            return -1;
        }
        rxMsgLen = (int)L;
    }

    if (hw.appBufAvailable() < rxMsgLen) { hw.unlock(); return 0; } // wait for the full payload

    int len = rxMsgLen;
    uint16_t expectedCrc = rxMsgCrc;
    rxMsgLen = -1;

    if (len > max_len) {
        // Caller's buffer is too small: drain to stay in sync, report error.
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
    hw.lock(); txBytes = 0; rxBytes = 0; totalErrs = 0; hw.unlock();
}

State ALink::getState() const { hw.lock(); State s = state; hw.unlock(); return s; }
int ALink::getErrCount() const { hw.lock(); int e = errs; hw.unlock(); return e; }
int ALink::getCurrentSpdIndex() const { hw.lock(); int idx = spdI; hw.unlock(); return idx; }

void ALink::onRx(const uint8_t* data, int len) {
    // One lock acquisition per UART event. The reliable-mode parser mutates
    // shared state (relRxBuf, relRxIdx, errs, rxBytes) and the app loop can be
    // reading/pushing the app buffer concurrently; serializing the whole event
    // against the app side is the simplest way to make the parser race-free.
    hw.lock();
    int i = 0;
    // Any RX activity in OK means the peer is alive; bump the idle watchdog.
    // Errors alone don't count -- the link is "alive enough to fail" and
    // only true silence should drop us.
    lastRxMs = hw.nowMs();
    // Set when err_unlocked() trips during this event; the BREAK is a wire
    // side effect and is emitted after the lock is released below.
    bool needSendBreak = false;
    while (i < len) {
        State cur_state = state;

        if (cur_state == State::OK) {
            if (cfg.reliableMode) {
                // UtilFrameRx validates frames and calls onPayload/onFrameError
                // below (still under the lock). It stops early if a callback
                // trips the err threshold; the outer loop then re-reads state
                // and hands the rest of the event to the command parser.
                i += frameRx.feed(data + i, len - i);
                if (state != State::OK) needSendBreak = true;
            } else {
                int n = len - i;
                int acc = hw.pushAppBuf(data + i, n);
                rxBytes += acc;
                if (acc < n && err_unlocked()) needSendBreak = true;
                i = len;
            }
        }
        else {
            uint8_t b = data[i++];
            if (rxIdx == 0 && b != 0xAA) continue;
            if (rxIdx == 1 && b != 0x55) { rxIdx = 0; continue; }
            rxBuf[rxIdx++] = b;

            if (rxIdx == 4) {
                rxIdx = 0;
                if (UtilCrc::crc8(rxBuf, 3) == rxBuf[3]) {
                    uint8_t payload = rxBuf[2];
                    if (cur_state == State::SWP) {
                        if (!isMaster && payload == REQ_CMD) {
                            int best = bestSpd_unlocked();
                            sendFrame_unlocked(best);
                            hw.setSpd(cfg.allowedBauds[best]);
                            errs = 0;
                            lastRxMs = lastTxMs = hw.nowMs();
                            changeState_unlocked(State::OK);
                            if (cfg.idleTimeoutMs > 0) hw.startTimer(okTickMs());
                        } else if (payload == PING_CMD && spdI < (int)cfg.allowedBauds.size()) {
                            scores[spdI]++;
                            spdI++;
                            // Retune in lockstep with the master so the next
                            // PING (at the next baud) can be decoded.
                            int next = (spdI < (int)cfg.allowedBauds.size()) ? spdI : 0;
                            hw.setSpd(cfg.allowedBauds[next]); // baud[0] readies us for REQ
                        }
                    }
                    else if (cur_state == State::LCK) {
                        if (isMaster) {
                            if (payload < (int)cfg.allowedBauds.size()) {
                                hw.setSpd(cfg.allowedBauds[payload]);
                                errs = 0;
                                lastRxMs = lastTxMs = hw.nowMs();
                                changeState_unlocked(State::OK);
                                if (cfg.idleTimeoutMs > 0) hw.startTimer(okTickMs());
                            }
                        } else {
                            if (payload == REQ_CMD) {
                                int best = bestSpd_unlocked();
                                sendFrame_unlocked(best);
                                hw.setSpd(cfg.allowedBauds[best]);
                                errs = 0;
                                lastRxMs = lastTxMs = hw.nowMs();
                                changeState_unlocked(State::OK);
                                if (cfg.idleTimeoutMs > 0) hw.startTimer(okTickMs());
                            }
                        }
                    }
                }
            }
        }
    }
    hw.unlock();
    // If the parser tripped the err threshold while holding the lock, tell
    // the peer to re-sweep now that we're back outside it. dropLink_unlocked
    // already ran from inside err_unlocked, so the local side is in SWP
    // listening at allowedBauds[0] -- the BREAK is the "come find me" ping.
    if (needSendBreak) hw.sendBreak();
}

// Reset all per-link state and retune to allowedBauds[0]. Idempotent.
// Caller must hold the lock; it is held throughout (HAL calls never take it).
// Master arms the sweep timer; slave waits passively.
//
// `countAsError` bumps the lifetime error counter. The link can drop for
// reasons that aren't "errors" (cold start, the master's own REQ-timeout
// restart) and reasons that are (bad frame flood tripping the threshold,
// peer BREAK, idle watchdog). Passing false for the init/RE paths keeps
// the lifetime counter aligned with what a human would call "an error".
void ALink::dropLink_unlocked(bool countAsError) {
    if (countAsError) totalErrs++;
    changeState_unlocked(State::SWP);
    spdI = 0;
    rxIdx = 0;
    rxMsgLen = -1;
    frameRx.reset();
    for (int i = 0; i < (int)scores.size(); i++) scores[i] = 0;
    errs = 0;
    lckRetries = 0;
    lastRxMs = hw.nowMs();
    hw.clearAppBuf();
    hw.setSpd(cfg.allowedBauds[0]);
    if (isMaster) hw.startTimer(cfg.delayMs);
    else hw.stopTimer();
}

// UtilFrameRx::Listener -- called (under the lock) per validated payload.
// Returns true if the err threshold tripped and the link dropped.
bool ALink::onPayload(const uint8_t* b, int n) {
    int acc = hw.pushAppBuf(b, n);
    rxBytes += acc;
    if (acc < n) {
        // App buffer full: bytes lost, stream desynced. Count the loss.
        return err_unlocked();
    }
    return false;
}

// UtilFrameRx::Listener -- corrupt/oversize/desynced frame.
bool ALink::onFrameError() {
    return err_unlocked();
}

void ALink::onBreak() {
    hw.lock();
    Log::getLog().info(ALINK_TAG, "BREAK received -> re-sweep");
    // Peer-initiated drop: their link died, or we were so out of sync they
    // had to re-sweep us. Count it -- from our side the link just failed.
    dropLink_unlocked(true);
    hw.unlock();
}

void ALink::onTimer() {
    hw.lock();
    State s = state;
    int curSpd = spdI;

    // OK: idle watchdog + keepalive, then re-arm the tick. The watchdog is
    // how the master notices a silently dead slave (its RX pin just goes
    // quiet); the keepalive is what stops a healthy-but-quiet link from
    // tripping the peer's watchdog.
    if (s == State::OK) {
        if (cfg.idleTimeoutMs > 0) {
            uint32_t now = hw.nowMs();
            if ((uint32_t)(now - lastRxMs) > (uint32_t)cfg.idleTimeoutMs) {
                Log::getLog().info(ALINK_TAG, "Idle for %u ms (limit %d) -> dropping link",
                                   (unsigned)(now - lastRxMs), cfg.idleTimeoutMs);
                // Silent peer death: a link failure from our point of view
                // even though recovery is clean. Counts as an error.
                dropLink_unlocked(true);
                hw.unlock();
                hw.sendBreak();   // tell the peer to re-sweep too
                return;
            }
            // Keepalive: a lone 0x00 the peer's reliable parser skips as a
            // stray inter-frame zero, but which still feeds its watchdog.
            // Raw mode would see it as data, so it only runs in reliable mode.
            if (cfg.reliableMode && (uint32_t)(now - lastTxMs) >= (uint32_t)okTickMs()) {
                uint8_t z = 0;
                hw.tx(&z, 1);
                lastTxMs = now;
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
        if (isMaster && curSpd < (int)cfg.allowedBauds.size()) sendFrame(PING_CMD);

        hw.lock();
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
            hw.unlock();
            hw.setSpd(cfg.allowedBauds[0]);
            if (isMaster) hw.startTimer(cfg.delayMs);
        }
    }
    else if (s == State::LCK && isMaster) {
        sendFrame(REQ_CMD);
        hw.lock();
        lckRetries++;
        hw.unlock();
        // If we've sent REQ too many times with no slave response, the peer
        // is gone -- restart the sweep from scratch. The next sweep will
        // either find a freshly-booted slave at 9600 or give up again.
        if (lckRetries > (int)cfg.allowedBauds.size() * 2) {
            Log::getLog().info(ALINK_TAG, "LCK timeout: no peer reply after %d REQs -> re-sweep",
                               lckRetries);
            hw.lock();
            // No slave answered REQ -- either the peer is dead or its baud
            // drifted. From our side the link just failed. Counts.
            dropLink_unlocked(true);
            hw.unlock();
            hw.sendBreak();
            return;
        }
        hw.startTimer(cfg.delayMs);
    }
}

} // namespace autolink
