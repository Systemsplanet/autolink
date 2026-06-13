// ALink.cpp — AutoLink protocol core implementation.
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
    Log::getLog().info(ALINK_TAG, "Initialized as %s. Reliable Mode: %s",
                       isMaster ? "Ping" : "Pong", cfg.reliableMode ? "ON" : "OFF");
    if (cfg.maxMsg > cfg.streamBufferSize) {
        Log::getLog().error(ALINK_TAG,
            "maxMsg (%u) > streamBufferSize (%u): large messages cannot be reassembled",
            (unsigned)cfg.maxMsg, (unsigned)cfg.streamBufferSize);
    }
}

void ALink::begin() {
    // Log the full baud sweep config so connection issues are immediately visible.
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
        // Run the local reset explicitly: a sender never receives its own
        // BREAK. First init -- this is NOT a disconnect event, so use
        // reset_unlocked() instead of dropLink_unlocked().
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
        hw.unlock();
        hw.clearAppBuf();
        hw.setSpd(cfg.allowedBauds[spdI]);
        Log::getLog().info(ALINK_TAG, "SWP Pong testing baud[0]=%lu",
            (unsigned long)cfg.allowedBauds[0]);
        // Pong sweep timer: fires every pingSamplesPerBaud*delayMs so the
        // Pong advances in lockstep with the Ping even when a baud is too
        // fast for the physical wiring and produces zero decodable PINGs.
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

// Pick the most reliable baud using the reliability sweep. See
// UtilBaudSweep::pickBest() for the full algorithm. Returns 0 if the sweep
// produced no data at all (a fresh negotiation that never saw a PING).
int ALink::bestSpd_unlocked() const {
    int best = baudSweep.pickBest();
    if (best < 0) return 0;
    // If a faster baud also received PINGs, prefer it even if it didn't reach
    // the strict threshold. Late SWP entry causes early PINGs at 115200 to be
    // missed; the fastest physically-reachable baud is always the right choice.
    for (int j = 0; j < best; j++) {
        if (baudSweep.scoreAt(j) > 0) return j;
    }
    return best;
}

void ALink::sendFrame(uint8_t payload) {
    uint8_t frame[4] = {0xAA, 0x55, payload, 0};
    frame[3] = UtilCrc::crc8(frame, 3);
    hw.lock();
    if (hw.tx(frame, 4) != 4)
        Log::getLog().error(ALINK_TAG, "sendFrame TX truncated (payload=0x%02X)", payload);
    hw.flushTx();
    hw.unlock();
}

// Caller must already hold the lock. Used from the locked onRx path.
void ALink::sendFrame_unlocked(uint8_t payload) {
    uint8_t frame[4] = {0xAA, 0x55, payload, 0};
    frame[3] = UtilCrc::crc8(frame, 3);
    if (hw.tx(frame, 4) != 4)
        Log::getLog().error(ALINK_TAG, "sendFrame_unlocked TX truncated (payload=0x%02X)", payload);
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
// `errs` (the threshold window) is the only thing this function touches --
// the lifetime disconnect counter is bumped by dropLink_unlocked() itself,
// so a single disconnect is one count regardless of how many err() calls
// the parser made along the way. Per-byte noise during SWP/LCK is
// intentionally not counted.
bool ALink::err_unlocked() {
    if (state != State::OK) return false;
    errs++;
    lifetimeErrs++;   // cumulative; never reset by clearErr() or lock
    if (errs > cfg.errThreshold) {
        Log::getLog().info(ALINK_TAG, "Error threshold exceeded (%d > %d). Dropping link.",
                           errs, cfg.errThreshold);
        dropLink_unlocked();   // counts as one disconnect event
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
    uint8_t unenc[MAX_CHUNK + 1];
    uint8_t frame[MAX_CHUNK + 5];
    while(offset < len) {
        int chunk = std::min(len - offset, MAX_CHUNK);
        memcpy(unenc, b + offset, chunk);
        unenc[chunk] = UtilCrc::crc8(unenc, chunk);
        frame[0] = 0x00;
        size_t encLen = UtilCobs::encode(unenc, chunk + 1, frame + 1);
        frame[1 + encLen] = 0x00;

        hw.lock();
        if (state != State::OK) { hw.unlock(); break; }
        int frameLen = (int)(encLen + 2);
        int sent = hw.tx(frame, frameLen);
        if (sent != frameLen) {
            // TX ring was full: COBS frame truncated. The receiver gets a bad
            // CRC8, counts a frame error, and eventually drops. Log and count
            // so repeated truncations trip errThreshold cleanly. Root cause is
            // txBufferSize too small for maxMsg — see AutoLinkConfig.
            Log::getLog().error(ALINK_TAG,
                "TX truncated: frame wanted %d bytes, UART accepted %d "
                "(txBufferSize too small for maxMsg=%zu?)",
                frameLen, sent, cfg.maxMsg);
            err_unlocked();
            hw.unlock();
            break;
        }
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
        int frameLen = (int)(encLen + 2);
        int sent = hw.tx(frame, frameLen);
        if (sent != frameLen) {
            Log::getLog().error(ALINK_TAG,
                "TX truncated (locked): frame wanted %d, UART accepted %d", frameLen, sent);
            err_unlocked();
            break;
        }
        txBytes += chunk; lastTxMs = hw.nowMs();
        offset += chunk;
    }
    return offset;
}

void ALink::dropLink() {
    hw.lock();
    dropLink_unlocked();
    bool needBreak = (state == State::SWP);   // dropLink_unlocked moves to SWP
    hw.unlock();
    if (needBreak) hw.sendBreak();   // wake the peer
}

void ALink::flush() { hw.flushTx(); }

void ALink::flushRx() {
    // Two-stage flush: stream buffer (ALink app layer) then UART driver ring
    // (hardware layer). Doing only the stream buffer is insufficient: the
    // UART event task immediately refills the stream buffer from the driver
    // ring, so the very next recvMsg call reads the same stale bytes that
    // caused the reject in the first place. Both layers must be cleared.
    hw.lock();
    int app_bytes = hw.appBufAvailable();
    hw.clearAppBuf();
    rxMsgLen = -1;
    hw.unlock();
    // Flush the UART ring outside the lock; uart_flush_input has its own
    // driver-level locking and does not need the ALink protocol lock.
    hw.flushRxHw();
    Log::getLog().debug(ALINK_TAG,
        "flushRx: cleared %d stream bytes + hw ring (rxMsgLen reset)",
        app_bytes);
}

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
    // Zeros tx/rx only. The lifetime error counter is independent --
    // sampling throughput for B/s must not wipe the very history that
    // would let you see "errors went up while I wasn't looking".
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

void ALink::onRx(const uint8_t* data, int len) {
    // One lock acquisition per UART event. The reliable-mode parser mutates
    // shared state (relRxBuf, relRxIdx, errs, rxBytes) and the app loop can be
    // reading/pushing the app buffer concurrently; serializing the whole event
    // against the app side is the simplest way to make the parser race-free.
    hw.lock();
    int i = 0;
    // Track raw bytes received in SWP for wiring diagnostics (logged each baud window).
    if (state == State::SWP) swpRxBytes_ += len;
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
                if (acc < n) {
                    if (err_unlocked()) needSendBreak = true;
                } else if (errs > 0) {
                    errs = 0;   // clean accept -> consecutive-error run is broken
                }
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
                            spdI = best;   // track the actual locked baud index
                            errs = 0;
                            lastRxMs = lastTxMs = hw.nowMs();
                            changeState_unlocked(State::OK);
                            if (cfg.idleTimeoutMs > 0) hw.startTimer(okTickMs());
                        } else if (isMaster && cfg.fastBaudLock
                                   && payload != PING_CMD && payload != REQ_CMD
                                   && payload < (int)cfg.allowedBauds.size()) {
                            // Fast-ack from the Pong: it has enough
                            // confidence at the current baud and is
                            // telling us to lock here. Same frame format
                            // as the existing LCK best-reply, so the
                            // Ping just treats any in-range payload
                            // (that's not PING/REQ) as a best-ack.
                            hw.setSpd(cfg.allowedBauds[payload]);
                            spdI = (int)payload;  // track the actual locked baud index
                            errs = 0;
                            lastRxMs = lastTxMs = hw.nowMs();
                            Log::getLog().info(ALINK_TAG, "Locked at %lu baud (fast-ack)",
                                               (unsigned long)cfg.allowedBauds[payload]);
                            changeState_unlocked(State::OK);
                            // Re-arm the same timer as the OK watchdog/keepalive
                            // clock. Do NOT stopTimer() here: that was killing the
                            // idle watchdog, so a Ping that fast-ack locked could
                            // sit in OK forever after the peer rebooted into SWP
                            // (rx=0, never re-sweeping). The watchdog firing on no
                            // RX is exactly what drops the stale link and re-PINGs.
                            if (cfg.idleTimeoutMs > 0) hw.startTimer(okTickMs());
                        } else if (payload == PING_CMD && spdI < (int)cfg.allowedBauds.size()) {
                            baudSweep.score(spdI);
                            // Fast-ack path: if we've scored enough PINGs
                            // at the current baud to trust it, send a
                            // best-ack immediately and lock. The Ping
                            // is at the same baud (lockstep), so it sees
                            // the ack and jumps to OK. Same one-frame
                            // format as the existing LCK best-reply, so
                            // the Ping just treats any in-range payload
                            // in SWP the same way it does in LCK.
                            if (cfg.fastBaudLock && baudSweep.scoreAt(spdI) >= baudSweep.minHitsForReliable()) {
                                int best = baudSweep.pickBest();
                                if (best < 0) best = spdI;
                                // Even if pickBest() returned a slower baud (it hit the
                                // threshold while faster bauds only scored partially),
                                // prefer the fastest baud that received ANY PINGs.
                                // Timing jitter causes late SWP entry to miss early PINGs
                                // at 115200, but the physical link is still fast — locking
                                // at a slower baud just because 19200 hit the threshold
                                // first is always the wrong outcome.
                                for (int j = 0; j < best; j++) {
                                    if (baudSweep.scoreAt(j) > 0) { best = j; break; }
                                }
                                sendFrame_unlocked((uint8_t)best);
                                hw.setSpd(cfg.allowedBauds[best]);
                                spdI = best;   // track the actual locked baud index
                                errs = 0;
                                lastRxMs = lastTxMs = hw.nowMs();
                                Log::getLog().info(ALINK_TAG, "Locked at %lu baud (fast-ack)",
                                                   (unsigned long)cfg.allowedBauds[best]);
                                changeState_unlocked(State::OK);
                                if (cfg.idleTimeoutMs > 0) hw.startTimer(okTickMs());
                            } else {
                                // Reliability sweep: stay at this baud for
                                // `pingSamplesPerBaud` PINGs so the Pong
                                // can score the decode rate. Only advance
                                // spdI and retune when we have a full sample.
                                int samples = baudSweep.samplesPerBaud();
                                if (samples < 1) samples = 1;
                                // Log the first decoded PING at each baud to
                                // confirm the physical layer is working there.
                                if (baudSweep.scoreAt(spdI) == 1) {
                                    Log::getLog().info(ALINK_TAG,
                                        "SWP Pong: first PING at baud[%d]=%lu",
                                        spdI, (unsigned long)cfg.allowedBauds[spdI]);
                                }
                                if (++pingSample >= samples) {
                                    pingSample = 0;
                                    spdI++;
                                    int next = (spdI < (int)cfg.allowedBauds.size()) ? spdI : 0;
                                    hw.setSpd(cfg.allowedBauds[next]);
                                    // Reset the baud-window timer so the Ping
                                    // gets a fresh window at the new baud.
                                    hw.startTimer(cfg.pingSamplesPerBaud * cfg.delayMs);
                                }
                            }
                        }
                    }
                    else if (cur_state == State::LCK) {
                        if (isMaster) {
                            if (payload < (int)cfg.allowedBauds.size()) {
                                hw.setSpd(cfg.allowedBauds[payload]);
                                spdI = (int)payload;  // track the actual locked baud index
                                errs = 0;
                                lastRxMs = lastTxMs = hw.nowMs();
                                Log::getLog().info(ALINK_TAG, "Locked at %lu baud",
                                                   (unsigned long)cfg.allowedBauds[payload]);
                                changeState_unlocked(State::OK);
                                if (cfg.idleTimeoutMs > 0) hw.startTimer(okTickMs());
                            }
                        } else {
                            if (payload == REQ_CMD) {
                                int best = bestSpd_unlocked();
                                sendFrame_unlocked(best);
                                hw.setSpd(cfg.allowedBauds[best]);
                                spdI = best;   // track the actual locked baud index
                                errs = 0;
                                lastRxMs = lastTxMs = hw.nowMs();
                                Log::getLog().info(ALINK_TAG, "Locked at %lu baud",
                                                   (unsigned long)cfg.allowedBauds[best]);
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
// Ping arms the sweep timer; Pong waits passively. Counts as one
// disconnect event for the lifetime error counter; use reset_unlocked()
// from begin() to do the same work without counting.
void ALink::dropLink_unlocked() {
    // Count exactly one event per OK -> SWP transition. While already in
    // SWP/LCK, additional onBreak() calls, threshold trips, and LCK timeouts
    // are all part of the same recovery (e.g. a Pong that emits multiple
    // BREAKs while rebooting) and must not inflate the count. The user's
    // mental model: "one Pong reset = one count, no matter how noisy the
    // recovery was."
    if (state == State::OK) totalErrs++;
    changeState_unlocked(State::SWP);
    // Sweep in array order: see begin()'s Pong branch for the
    // rationale. The lockstep is preserved.
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
    hw.clearAppBuf();
    hw.setSpd(cfg.allowedBauds[spdI]);
    if (isMaster) hw.startTimer(cfg.delayMs);
    else          hw.startTimer(cfg.pingSamplesPerBaud * cfg.delayMs);
}

// Same as dropLink_unlocked but does NOT bump the lifetime error counter.
// Used by begin() to do the initial local reset on startup.
void ALink::reset_unlocked() {
    changeState_unlocked(State::SWP);
    // Sweep in array order: see begin()'s Pong branch for the
    // rationale. The lockstep is preserved.
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
    hw.clearAppBuf();
    hw.setSpd(cfg.allowedBauds[spdI]);
    if (isMaster) hw.startTimer(cfg.delayMs);
    else          hw.startTimer(cfg.pingSamplesPerBaud * cfg.delayMs);
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
    // A good frame proves the link is healthy. errThreshold counts
    // *consecutive* failures, not lifetime ones -- a handful of CRC rejects
    // scattered across hundreds of good frames is ordinary RF noise and must
    // not drop a working link. Clear the counter so only a genuine run of
    // back-to-back errors (a truly broken line) trips the threshold.
    if (errs > 0) errs = 0;
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
    // had to re-sweep us. One disconnect event.
    dropLink_unlocked();
    hw.unlock();
}

void ALink::onTimer() {
    hw.lock();
    State s = state;
    int curSpd = spdI;

    // OK: idle watchdog + keepalive, then re-arm the tick. The watchdog is
    // how the Ping notices a silently dead Pong (its RX pin just goes
    // quiet); the keepalive is what stops a healthy-but-quiet link from
    // tripping the peer's watchdog.
    if (s == State::OK) {
        if (cfg.idleTimeoutMs > 0) {
            uint32_t now = hw.nowMs();
            if ((uint32_t)(now - lastRxMs) > (uint32_t)cfg.idleTimeoutMs) {
                Log::getLog().info(ALINK_TAG, "Idle for %u ms (limit %d) -> dropping link",
                                   (unsigned)(now - lastRxMs), cfg.idleTimeoutMs);
                // Silent peer death: a link failure. One disconnect event.
                dropLink_unlocked();
                hw.unlock();
                hw.sendBreak();   // tell the peer to re-sweep too
                return;
            }
            // Keepalive: 0x00 0x00 — a self-resyncing empty frame. See
            // UtilFrameRx::feed(). Emitted only in OK under lock, so the
            // sender is always at a frame boundary; the receiver
            // interprets the pair at a clean boundary as a no-op or as
            // a one-error partial-flush mid-COBS. Reliable mode only.
            if (cfg.reliableMode && (uint32_t)(now - lastTxMs) >= (uint32_t)okTickMs()) {
                uint8_t ka[2] = { 0x00, 0x00 };
                if (hw.tx(ka, 2) != 2)
                    Log::getLog().error(ALINK_TAG, "keepalive TX truncated");
                else
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
        // Reliability sweep: send `pingSamplesPerBaud` PINGs at each
        // baud in array order, so the Pong can score decode rates.
        // The Ping stays on the same baud for N consecutive timer
        // ticks (driven by `pingSample`); only when the local count
        // reaches samplesPerBaud does the Ping advance spdI and
        // retune. Once we've covered the whole list without the Pong
        // acking, fall through to LCK and use the legacy REQ_CMD path.
        if (isMaster && curSpd < (int)cfg.allowedBauds.size()) {
            // Log once at the start of each baud window so connection issues
            // are easy to spot in the serial monitor.
            if (pingSample == 0) {
                Log::getLog().info(ALINK_TAG, "SWP Ping baud[%d]=%lu",
                    curSpd, (unsigned long)cfg.allowedBauds[curSpd]);
            }
            sendFrame(PING_CMD);
            int samples = baudSweep.samplesPerBaud();
            if (samples < 1) samples = 1;
            if (pingSample + 1 >= samples) {
                // Done with this baud. Increment spdI, retune (or
                // transition to LCK if we've covered the whole list).
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
                    spdI = 0;  // UART will be tuned to allowedBauds[0] below
                    hw.unlock();
                    hw.setSpd(cfg.allowedBauds[0]);
                    if (isMaster) hw.startTimer(cfg.delayMs);
                }
            } else {
                // Stay on this baud; re-arm the timer.
                hw.lock();
                pingSample++;
                hw.unlock();
                hw.startTimer(cfg.delayMs);
            }
        } else if (!isMaster) {
            // Pong sweep timer: one baud window (pingSamplesPerBaud * delayMs)
            // has elapsed. If we didn't score enough PINGs to fast-ack, advance
            // to the next baud so the Pong stays in lockstep with the Ping
            // even when the current baud is too fast for the physical wiring.
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
                    swpRxBytes_ = 0;  // reset for the next baud window
                    spdI++;
                    if (spdI >= (int)cfg.allowedBauds.size()) {
                        // Check whether any baud decoded even one PING before resetting
                        // scores. If not, the physical layer is broken (wrong pins or
                        // wrong wiring) — log a prominent warning.
                        bool anyPinged = false;
                        for (int i = 0; i < (int)cfg.allowedBauds.size(); i++) {
                            if (baudSweep.scoreAt(i) > 0) { anyPinged = true; break; }
                        }
                        Log::getLog().info(ALINK_TAG,
                            "SWP Pong: full sweep done, restarting from baud[0]=%lu",
                            (unsigned long)cfg.allowedBauds[0]);
                        spdI = 0;
                        baudSweep.resetAll();  // fresh scores for the next sweep

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
                hw.unlock();  // transitioned to OK/LCK under the lock; stop
            }
        }
    }
    else if (s == State::LCK && isMaster) {
        sendFrame(REQ_CMD);
        hw.lock();
        lckRetries++;
        hw.unlock();
        // If we've sent REQ too many times with no Pong response, the peer
        // is gone -- restart the sweep from scratch. The next sweep will
        // either find a freshly-booted Pong at 9600 or give up again.
        if (lckRetries > (int)cfg.allowedBauds.size() * 2) {
            Log::getLog().info(ALINK_TAG, "LCK timeout: no peer reply after %d REQs -> re-sweep",
                               lckRetries);
            hw.lock();
            // No Pong answered REQ -- either the peer is dead or its baud
            // drifted. One disconnect event.
            dropLink_unlocked();
            hw.unlock();
            hw.sendBreak();
            return;
        }
        hw.startTimer(cfg.delayMs);
    }
}

} // namespace autolink
