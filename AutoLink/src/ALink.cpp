// ALink.cpp — AutoLink protocol core.
//
// State machine (SWP/LCK/OK), COBS+CRC-8 framing with cobsSeq ordering,
// CRC-16 message layer, auto-baud sweep with reliability scoring, idle
// watchdog, keepalive, and error thresholding. All physical I/O goes
// through the injected ILink so this file compiles and runs on the host
// for unit testing.
#include "ALink.h"
#include "Log.h"
#include <cstdio>      // snprintf (RX hex dump)

#ifdef ARDUINO
// portYIELD() is defined by FreeRTOS.h. Include it at file scope so
// its extern "C" blocks aren't nested inside namespace autolink.
#  if defined(ESP_PLATFORM) || defined(ESP32) || defined(ARDUINO_ARCH_ESP32)
#    include <freertos/FreeRTOS.h>
#  endif
#endif
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
    // Timer tick rate for the OK state. MUST be at least
    // ACK_RTO_MS so the retransmit scan runs often enough to catch
    // a lost wire frame within one RTO. The keepalive check inside
    // onTimerOk_unlocked uses cfg.idleTimeoutMs / 3 as its own
    // interval and is independent of the tick rate.
    int keep = cfg.idleTimeoutMs / 3;
    if (keep < 50) keep = 50;
    return keep < (int)ACK_RTO_MS ? (int)ACK_RTO_MS : keep;
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
      baudSweep((int)config.allowedBaudsCount),
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
    // lostMsgs is the lifetime wire-loss tally (dashboard "lost msgs"
    // card); survives drop/re-sweep on purpose. Zero via resetDiag().
    Log::getLog().debug(ALINK_TAG, "cobsSeq reset (sender=%u, lastRx cleared)", (unsigned)txSeq);
}

void ALink::begin() {
    // v5.1.14 (audit #6): the condition was inverted — the log used
    // to fire only when allowedBaudsCount == 0, which is the empty
    // case (and the log would also OOB-read allowedBauds[-1]). Flip
    // to != so the log fires whenever there's at least one baud to
    // sweep. Disclosed.
    if (cfg.allowedBaudsCount != 0) {
        Log::getLog().info(ALINK_TAG, "%s: %d bauds %lu..%lu, %d samples/baud, fastAck=%s",
            isMaster ? "Ping" : "Pong",
            (int)cfg.allowedBaudsCount,
            (unsigned long)cfg.allowedBauds[0],
            (unsigned long)cfg.allowedBauds[cfg.allowedBaudsCount-1],
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
        // DEBUG: transitions fire every baud tick during negotiation
        // (~80 lines for a 4 s SWP), which drowns the live log at INFO.
        // INFO keeps only user-meaningful events (link up/down, errors).
        Log::getLog().debug(ALINK_TAG, "State Transition: %s -> %s",
                            StateToStr(state), StateToStr(newState));
        state = newState;
    }
}

int ALink::bestSpd_unlocked() const {
    int best = baudSweep.pickBest();
    if (best < 0) return 0;
    // Prefer the highest baud with any decodes, falling back to the
    // scoring-based best.
    for (int j = 0; j < best; j++) {
        if (baudSweep.scoreAt(j) > 0) return j;
    }
    return best;
}

// Control frame: [0xAA, 0x55, cobsSeq, payload, CRC8(first-4)].
// cobsSeq is monotonic per-link and resets only on link drop.
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
    // No flushTx(): the auto-sized TX ring accepts 5 bytes instantly
    // and non-blocking, and the data path doesn't flush either. Users
    // who need an explicit drain have the public flush() entry point.
    // VERBOSE: fires on every control byte (PING/PONG/keepalive);
    // hundreds of lines per second at 115200 baud. Opt in via the
    // dashboard's Verbose radio for wire-trace forensics.
    Log::getLog().verbose(ALINK_TAG,
        "sendFrame TX  cobsSeq=%u  payload=0x%02X",
        (unsigned)txSeq, (unsigned)payload);
    // Don't bump txSeq for control frames: Pong replies with the same
    // cobsSeq so Ping can match request/ack. Only the data path
    // (sendCobsFrame_unlocked) consumes cobsSeq numbers.
}

void ALink::sendFrame(uint8_t payload) {
    hw.lock();
    sendFrame_unlocked(payload);
    hw.unlock();
}

// Reliable-mode data frame:
//   [0x00] [COBS(cobsSeq | payload) | CRC8(cobsSeq | payload)] [0x00]
// Each data frame consumes one cobsSeq. With n=0 this is the keepalive
// shape — a cobsSeq-bearing frame so the receiver's gap detection sees
// traffic even when the app has nothing to send.
void ALink::sendCobsFrame_unlocked(const uint8_t* b, int n) {
    if (n < 0) n = 0;
    if (n > MAX_CHUNK) n = MAX_CHUNK;

    uint8_t unenc[MAX_CHUNK + 3];
    unenc[0] = txSeq;
    if (n > 0) memcpy(unenc + 1, b, n);
    unenc[1 + n] = UtilCrc::crc8(unenc, 1 + n);
    size_t rawLen = (size_t)(1 + n) + 1;

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
    txSeq = (uint8_t)(txSeq + 1);
}

void ALink::sendCobsFrame(const uint8_t* b, int n) {
    hw.lock();
    sendCobsFrame_unlocked(b, n);
    hw.unlock();
}

// v5 ARQ: same wire frame as sendCobsFrame_unlocked, but ALSO marks
// the cobsSeq used as "waiting for ACK" in the pending map and
// stamps sentAtMs_ + zeros retxCount_.
//
// For multi-chunk messages, the caller passes the message's base
// cobsSeq (the one the facade cached the payload under). The protocol
// remembers it in baseSeq_[chunkSeq] so that when a chunk's ACK
// times out, the retransmit hook can look up the cache entry via
// the base seq and re-send the whole message. Without this, a
// chunk's retransmit looks up an empty cache slot and the link
// drops.
//
// Pass baseSeq == 0xFF (NO_BASE) for a single-frame message — the
// chunk IS the base, and the cache key is the chunk's cobsSeq.
//
// Caller holds the lock.
uint8_t ALink::sendCobsFrameAcked_unlocked(const uint8_t* b, int n, uint8_t baseSeq) {
    uint8_t seq = txSeq;
    sendCobsFrame_unlocked(b, n);  // bumps txSeq by 1 inside
    ackedPending_[seq] = true;
    retxCount_[seq]    = 0;
    sentAtMs_[seq]     = hw.nowMs();
    baseSeq_[seq]      = (baseSeq == NO_BASE) ? seq : baseSeq;
    return seq;
}

// Send a 1-byte-payload ACK frame acknowledging the given cobsSeq.
// Wire format: [0x00][COBS(ACK_TYPE | ackedSeq) | CRC8][0x00]. Caller
// holds the lock.
void ALink::sendAckFrame_unlocked(uint8_t ackedCobsSeq) {
    uint8_t unenc[3] = { ACK_TYPE, ackedCobsSeq, 0 };
    unenc[2] = UtilCrc::crc8(unenc, 2);
    uint8_t frame[8];
    frame[0] = 0x00;
    size_t encLen = UtilCobs::encode(unenc, 3, frame + 1);
    frame[1 + encLen] = 0x00;
    int sent = hw.tx(frame, (int)(encLen + 2));
    if ((int)(encLen + 2) != sent) {
        Log::getLog().error(ALINK_TAG,
            "sendAckFrame TX truncated (ackedSeq=%u accepted=%d)",
            (unsigned)ackedCobsSeq, sent);
    }
    // No cobsSeq bump — ACKs don't consume data-path sequence numbers.
    // No ACK of the ACK — that would loop forever.
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
    // DEBUG: per-frame-error counter for the operator. Threshold trip
    // stays at INFO (it's a state change worth highlighting).
    Log::getLog().debug(ALINK_TAG,
        "frame error #%d (cumulative frameErrs=%llu, threshold=%d)",
        errs, (unsigned long long)frameErrs, cfg.errThreshold);
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
    if (len <= 0) {
        // n=0 is a no-op (the keepalive path handles cobsSeq-only
        // frames). Negative len is a programmer error.
        if (len < 0) {
            Log::getLog().error(ALINK_TAG,
                "write rejected: len=%d (negative). No bytes sent.", len);
        }
        return 0;
    }
    hw.lock();
    if (state != State::OK) {
        // Warning: silent state-rejection was the source of the
        // "sendMsg returned but data never arrived" confusion.
        State s = state;
        hw.unlock();
        Log::getLog().warning(ALINK_TAG,
            "write rejected: link not in OK (state=%s), %d bytes dropped. "
            "Call dropLink() and wait for re-sweep, or check wiring/peer.",
            StateToStr(s), len);
        return 0;
    }

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
        if (state != State::OK) {
            // Bailed mid-message: peer sees a gap, partial is lost.
            State s = state;
            Log::getLog().warning(ALINK_TAG,
                "write aborted mid-message at offset=%d/%d (state=%s). "
                "Peer will see a gap; the partial message is lost.",
                offset, len, StateToStr(s));
            break;
        }
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
    // Reset cobsSeq receiver state so the next byte is treated as the
    // start of a fresh sequence (handles Pong->Ping link-up with stale
    // bytes from a previous session in the ring).
    rxSeqSet = false;
    rxSeq = 0;
    hw.unlock();
    hw.flushRxHw();
    Log::getLog().debug(ALINK_TAG,
        "flushRx: cleared %d stream bytes + hw ring (rxMsgLen + cobsSeq rx state reset)",
        app_bytes);
}

bool ALink::sendMsg(const uint8_t* b, int len) {
    if (len == 0) {
        // 0-byte sendMsg is a no-op (returns true). The keepalive
        // path handles cobsSeq-only frames separately.
        return true;
    }
    if (len < 0) {
        Log::getLog().error(ALINK_TAG,
            "sendMsg rejected: len=%d (negative). The 0-payload data-message "
            "path was removed; the keepalive handles cobsSeq advancement "
            "automatically in OK.", len);
        return false;
    }
    if ((size_t)len > cfg.maxMsg) {
        Log::getLog().error(ALINK_TAG,
            "sendMsg rejected: len=%d exceeds cfg.maxMsg=%u. Either shrink "
            "the message or raise cfg.maxMsg in AutoLinkConfig before "
            "constructing AutoLink.", len, (unsigned)cfg.maxMsg);
        return false;
    }

    uint16_t c = UtilCrc::crc16(b, len);
    uint8_t hdr[MSG_HDR] = {
        (uint8_t)(len), (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24),
        (uint8_t)(c), (uint8_t)(c >> 8)
    };

    hw.lock();
    if (state != State::OK) {
        State s = state;
        hw.unlock();
        Log::getLog().warning(ALINK_TAG,
            "sendMsg rejected: link not in OK (state=%s), %d bytes dropped.",
            StateToStr(s), len);
        return false;
    }

    // Use unlocked send paths — calling write() would re-enter the
    // non-recursive mutex and deadlock. Header and payload inside one
    // lock scope so no keepalive (timer task) interleaves between them.
    bool ok = true;
    if (cfg.reliableMode) {
        // v5 ARQ: every cobsSeq-bearing frame arms a retransmit
        // timer. The first byte of the header (L=lo) is the FIRST
        // cobsSeq; subsequent chunks increment by 1 each. If any of
        // those cobsSeq numbers aren't ACKed within ACK_RTO_MS, the
        // OK-state timer retransmits the WHOLE message (the facade
        // looks up the base seq in the cache and re-sends from
        // offset 0).
        uint8_t baseSeq = sendCobsFrameAcked_unlocked(hdr, MSG_HDR, NO_BASE);
        int offset = 0;
        while (offset < len) {
            if (state != State::OK) { ok = false; break; }
            int chunk = std::min(len - offset, MAX_CHUNK);
            sendCobsFrameAcked_unlocked(b + offset, chunk, baseSeq);
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
#ifdef ARDUINO
    // Yield to the scheduler. The Arduino loopTask runs at priority
    // 25 and a tight sendMsg burst can hold the CPU for ~50 ms per
    // message (UART TX-FIFO drain). Without a yield the httpd task
    // at priority 10 is starved for the entire burst and the
    // dashboard's /stats /logs /level calls time out. portYIELD()
    // is a no-op when no other task is ready (cheap), and gives
    // the httpd task a turn when it is. FreeRTOS.h is included at
    // file scope (top of ALink.cpp) so this call compiles on every
    // Arduino-ESP32 core/combo. No-op on host.
    portYIELD();
#endif
    return ok;
}

// Scan the app buffer forward from a corrupt header looking for the
// next plausibly-valid MSG_HDR boundary (L in [1, cfg.maxMsg]). The
// CRC is over the payload, not the header, so we can't fully validate
// here — the next recvMsg will fail with a CRC mismatch and re-resync.
// Bound `max_scan` keeps the worst-case search bounded.
//
// Snapshots (max_scan + MSG_HDR) bytes from the app buffer, scans the
// snapshot, and (on success) re-pushes the suffix starting at the
// resync point. On failure re-pushes the whole snapshot so the caller's
// clearAppBuf sees the right bytes.
int ALink::findMsgHeaderResync_unlocked(int max_scan) {
    int avail = hw.appBufAvailable();
    if (avail < MSG_HDR) return -1;
    int scan = (max_scan > 0 && max_scan < avail) ? max_scan : avail;
    int snapLen = scan + MSG_HDR;
    if (snapLen > avail) snapLen = avail;
    uint8_t* snap = (uint8_t*)malloc(snapLen);
    if (!snap) return -1;
    int got = hw.popAppBuf(snap, snapLen);
    int validLen = got;
    Log::getLog().debug(ALINK_TAG,
        "findMsgHeaderResync: avail=%d snapLen=%d got=%d validLen=%d",
        avail, snapLen, got, validLen);
    for (int drop = 0; drop + MSG_HDR <= validLen; drop++) {
        uint32_t L = (uint32_t)snap[drop] |
                     ((uint32_t)snap[drop + 1] << 8) |
                     ((uint32_t)snap[drop + 2] << 16) |
                     ((uint32_t)snap[drop + 3] << 24);
        if (L < 1 || L > cfg.maxMsg) continue;
        Log::getLog().debug(ALINK_TAG,
            "findMsgHeaderResync: candidate drop=%d L=%u", drop, (unsigned)L);
        hw.pushAppBuf(snap + drop, validLen - drop);
        free(snap);
        return drop;
    }
    // No resync point. Re-push the whole snapshot.
    hw.pushAppBuf(snap, validLen);
    free(snap);
    return -1;
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
        // Corrupt header: scan forward for the next valid MSG_HDR
        // boundary. If none found within maxMsg + MSG_HDR bytes
        // (one full legitimate message + header), the buffer is
        // genuinely garbage — clear it. This breaks the
        // cascading-desync the old "drop one header and hope" path
        // caused when a cobsSeq forward-gap resync realigned the wire
        // mid-payload.
        if (L == 0 || L > cfg.maxMsg) {
            int drop = findMsgHeaderResync_unlocked(cfg.maxMsg + MSG_HDR);
            if (drop >= 0) {
                if (drop > 0) {
                    Log::getLog().error(ALINK_TAG,
                        "recvMsg: corrupt MSG_HDR (L=%u) — resynced forward "
                        "by %d bytes to the next valid header. Lost %d bytes "
                        "of stream.", (unsigned)L, drop, drop);
                    hw.unlock();
                    err();
                    return -1;
                }
                // drop == 0: the bytes just past the head form a plausible L.
                // This is a benign self-correction, NOT a wire error — we
                // didn't drop anything. Don't bump frameErrs. Just return
                // -1 and let the next recvMsg re-read from there.
                Log::getLog().debug(ALINK_TAG,
                    "recvMsg: corrupt MSG_HDR (L=%u) at the very start of "
                    "the scan window — the bytes just past it form a "
                    "plausible L. Returning -1; the next recvMsg will "
                    "re-read from there.", (unsigned)L);
                hw.unlock();
                return -1;
            }
            int cleared = hw.appBufAvailable();
            hw.clearAppBuf();
            rxMsgLen = -1;
            Log::getLog().error(ALINK_TAG,
                "recvMsg: corrupt MSG_HDR (L=%u) and no resync point found "
                "within %d bytes — cleared the app buffer (%d bytes dropped). "
                "Link stays OK; the next sendMsg from the peer will be "
                "received cleanly.", (unsigned)L, cfg.maxMsg + MSG_HDR, cleared);
            hw.unlock();
            // Only bump frameErrs if we actually dropped real bytes.
            // 0-bytes-dropped is a benign case (buffer is genuinely
            // empty, resync was just confirming it).
            if (cleared > 0) err();
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

void ALink::resetDiag() {
    // Dashboard Reset clears the cobsSeq diagnostic counters too so
    // the "X lost msgs" pill doesn't stay at its lifetime value forever.
    hw.lock(); gaps = 0; stale = 0; lostMsgs = 0; hw.unlock();
}

State ALink::getState() const { hw.lock(); State s = state; hw.unlock(); return s; }
int ALink::getErrCount() const { hw.lock(); int e = errs; hw.unlock(); return e; }
int ALink::getCurrentSpdIndex() const { hw.lock(); int idx = spdI; hw.unlock(); return idx; }
uint32_t ALink::getCurrentBaud() const {
    hw.lock();
    uint32_t b = (spdI >= 0 && spdI < (int)cfg.allowedBaudsCount)
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

// lockOk_unlocked — single source of truth for "enter OK at baud N".
// All five callsites that used to paste the 6-line sequence funnel
// through here. Caller holds the lock.
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
            // Control-frame accumulator. Strips 0xAA 0x55 preamble,
            // fills rxBuf, validates CRC, dispatches at full size.
            uint8_t b = data[i++];
            if (rxIdx == 0 && b != 0xAA) continue;
            if (rxIdx == 1 && b != 0x55) { rxIdx = 0; continue; }
            rxBuf[rxIdx++] = b;

            if (rxIdx == CTRL_FRAME_SIZE) {
                rxIdx = 0;
                if (UtilCrc::crc8(rxBuf, CTRL_FRAME_SIZE - 1) != rxBuf[CTRL_FRAME_CRC_IDX]) {
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
        // Pong saw Ping's REQ at the top of the sweep: pick best baud,
        // tell Ping, both sides enter OK at that baud.
        int best = bestSpd_unlocked();
        sendFrame_unlocked((uint8_t)best);
        lockOk_unlocked(best, "REQ");
        return false;
    }

    if (isMaster && cfg.fastBaudLock
        && payload != PING_CMD && payload != REQ_CMD
        && payload < (int)cfg.allowedBaudsCount) {
        // Pong sent a fast-ack with a baud index instead of a PING/REQ.
        lockOk_unlocked((int)payload, "fast-ack");
        return false;
    }

    if (payload == PING_CMD && spdI < (int)cfg.allowedBaudsCount) {
        baudSweep.score(spdI);
        if (cfg.fastBaudLock && baudSweep.scoreAt(spdI) >= baudSweep.minHitsForReliable()) {
            int best = bestSpd_unlocked();
            sendFrame_unlocked((uint8_t)best);
            lockOk_unlocked(best, "fast-ack");
            return false;
        }
        // Not enough samples yet; advance after the last sample in
        // this window so the next baud is tested next tick.
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
            int next = (spdI < (int)cfg.allowedBaudsCount) ? spdI : 0;
            hw.setSpd(cfg.allowedBauds[next]);
            hw.startTimer(cfg.pingSamplesPerBaud * cfg.delayMs);
        }
        return false;
    }
    return false;
}

bool ALink::handleLck_unlocked(uint8_t cobsSeq, uint8_t payload) {
    (void)cobsSeq;   // LCK uses payload as a baud index; cobsSeq is just echoed back
    if (isMaster) {
        // Pong's REQ-reply carries a baud index. Lock there.
        if (payload < (int)cfg.allowedBaudsCount) {
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

// Reset all link state, retune to allowedBauds[0]. `count` picks
// between "this is a disconnect" (bumps discCount, called from
// err_unlocked, dropLink, onBreak) and "this is a fresh start" (no
// count, called from begin).
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
    // cobsSeq sender + receiver reset on every drop. After re-sweep
    // both sides restart from cobsSeq=0, so stale bytes from the
    // previous session are immediately rejected as gaps.
    resetSeq_unlocked();
    hw.clearAppBuf();
    hw.setSpd(cfg.allowedBauds[spdI]);
    if (isMaster) hw.startTimer(cfg.delayMs);
    else          hw.startTimer(cfg.pingSamplesPerBaud * cfg.delayMs);
}

// UtilFrameRx::Listener callback for validated reliable-mode frames.
// cobsSeq is the first decoded byte; gap/stale detection happens here
// before any bytes reach the app buffer, so a wire-byte shift can
// never reach the message layer.
bool ALink::onPayload(uint8_t cobsSeq, const uint8_t* b, int n) {
    if (state != State::OK) {
        // Frame received while not in OK (race against a drop in
        // another task). Return true so feed() stops handing the
        // rest of this event to the parser — those bytes belong to
        // the stale session.
        Log::getLog().debug(ALINK_TAG,
            "RX cobsSeq=%u  %d payload bytes  DROPPED (state != OK)", (unsigned)cobsSeq, n);
        return true;
    }

    // v5 ARQ: a missing ACK would have caused the sender to
    // retransmit, so by the time we see cobsSeq=N the sender's
    // retransmits have already caught up. A "gap" here means the
    // retransmit also got lost — the link is in trouble. We log
    // it (still counts toward gaps/lostMsgs for diagnostics) but
    // we DON'T drop the link — the next retransmit will catch up.
    //
    // A "stale" (backwards-jump) means a duplicate ACK-driven
    // retransmit arrived after the original was acked. Drop it.
    if (rxSeqSet) {
        uint8_t expected = (uint8_t)(rxSeq + 1);
        if (cobsSeq != expected) {
            int diff = (int)cobsSeq - (int)rxSeq;
            if (diff < 0) diff += COBS_SEQ_WRAP;
            if (diff == 0 || diff > COBS_SEQ_WRAP / 2) {
                // Duplicate or wraparound. The original was already
                // acked; this is just a redundant retransmit. Drop
                // without acking again (the sender already saw our
                // first ACK and freed the slot).
                stale++;
                Log::getLog().debug(ALINK_TAG,
                    "RX cobsSeq=%u STALE: expected %u, last good=%u  %d bytes DROPPED (duplicate retransmit)",
                    (unsigned)cobsSeq, (unsigned)expected, (unsigned)rxSeq, n);
                return false;
            }
            gaps++;
            uint64_t skipped = (uint64_t)(diff - 1);
            lostMsgs += skipped;
            Log::getLog().warning(ALINK_TAG,
                "RX cobsSeq=%u GAP: expected %u, last good=%u  %d bytes accepted (gap=%d, +%llu lost)",
                (unsigned)cobsSeq, (unsigned)expected, (unsigned)rxSeq, n, diff,
                (unsigned long long)skipped);
        }
    }
    rxSeq = cobsSeq;
    rxSeqSet = true;

    int acc = hw.pushAppBuf(b, n);
    rxBytes += acc;
    if (acc < n) {
        // App-buffer-full — app is falling behind. We can't ACK
        // a frame we didn't fully deliver (the sender would free
        // its slot and the next retransmit would be a duplicate).
        // Hold the ACK: the sender's retransmit will retry, and
        // when the app catches up the next attempt will deliver
        // fully and we ACK then.
        Log::getLog().info(ALINK_TAG,
            "RX cobsSeq=%u app buffer full: wanted %d accepted %d (frame NOT acked, sender will retransmit). "
            "If this fires on the FIRST frame, check the EspHal boot log for "
            "xStreamBufferCreate failure (heap too small or streamBufferSize too large).",
            (unsigned)cobsSeq, n, acc);
        gaps++;
        return true;
    }
    // ACK the frame so the sender frees its retransmit slot.
    sendAckFrame_unlocked(cobsSeq);
    Log::getLog().debug(ALINK_TAG,
        "RX cobsSeq=%u  %d payload bytes  acked (rxBytes=%llu)",
        (unsigned)cobsSeq, n, (unsigned long long)rxBytes);
    // Hex dump of the first up-to-10 payload bytes, ASCII hex space-separated.
    // Gated by Log::verbose(): emitted only when level >= VERBOSE, so debug
    // and below stay clean.
    if (n > 0) {
        int hd = n < 10 ? n : 10;
        char hex[10 * 3 + 1] = {};
        for (int i = 0; i < hd; i++) {
            snprintf(hex + i * 3, 4, "%02X ", b[i]);
        }
        Log::getLog().verbose(ALINK_TAG,
            "RX hex first %d: %s", hd, hex);
    }
    if (errs > 0) errs = 0;
    return false;
}

bool ALink::onAck(uint8_t ackedCobsSeq) {
    if (state != State::OK) return false;
    if (!ackedPending_[ackedCobsSeq]) {
        // Duplicate ACK or ACK for a cobsSeq we never sent (shouldn't
        // happen after a fresh link, but can on wraparound). Drop.
        Log::getLog().debug(ALINK_TAG,
            "RX ACK for cobsSeq=%u DROPPED (not in pending map)", (unsigned)ackedCobsSeq);
        return false;
    }
    ackedPending_[ackedCobsSeq] = false;
    retxCount_[ackedCobsSeq]    = 0;
    // Look up the base seq for this chunk so the facade can decrement
    // the right chunks_left counter (or free the slot if the chunk
    // was the only one). For keepalives, baseSeq_[s] is the keepalive
    // cobsSeq itself (set by sendCobsFrameAcked_unlocked with
    // NO_BASE), and the facade ignores those — no cache entry was
    // created for a keepalive.
    uint8_t base = baseSeq_[ackedCobsSeq];
    baseSeq_[ackedCobsSeq] = 0;
    Log::getLog().verbose(ALINK_TAG,
        "RX ACK cobsSeq=%u (base=%u)  slot freed", (unsigned)ackedCobsSeq, (unsigned)base);
    // Notify the facade (AutoLink) so it can free its payload cache
    // slot. The callback may run user code (malloc/free); it runs
    // under our lock, which is the same lock the cache uses, so no
    // extra synchronization needed.
    if (arqAckCallback_) arqAckCallback_(base, arqCtx_);
    return false;
}

bool ALink::onFrameError() {
    Log::getLog().debug(ALINK_TAG, "frame error (corrupt COBS / bad CRC / oversize)");
    return err_unlocked();
}

void ALink::onBreak() {
    hw.lock();
    Log::getLog().info(ALINK_TAG, "BREAK received -> re-sweep");
    reset_unlocked(true);  // peer-driven BREAK is a real disconnect
    hw.unlock();
}

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
    if (cfg.reliableMode && (uint32_t)(now - lastTxMs) >= (uint32_t)(cfg.idleTimeoutMs / 3)) {
        // Keepalive shape: a cobsSeq-bearing 0-payload data frame.
        // Funneling through the real encoder means the keepalive can
        // never drift from the data path's wire format.
        // Note: the keepalive interval is cfg.idleTimeoutMs / 3
        // directly, NOT okTickMs() — okTickMs() is the timer tick
        // rate (driven by ACK_RTO_MS for retransmit scan), which is
        // much faster than the keepalive interval.
        sendCobsFrame_unlocked(nullptr, 0);
        lastTxMs = now;
    }
    // v5 ARQ: notify the facade hook for any expired slot. The
    // facade (AutoLink) retransmits the cached payload from its
    // own state. If the facade returns true (= "drop the link"),
    // we tear down.
    if (cfg.reliableMode) {
        for (int s = 0; s < 256; s++) {
            if (!ackedPending_[s]) continue;
            uint32_t age = now - sentAtMs_[s];
            if (age < ACK_RTO_MS) continue;
            if (retxCount_[s] >= MAX_RETX) {
                Log::getLog().error(ALINK_TAG,
                    "cobsSeq=%u exceeded MAX_RETX=%d (lost wire) -> dropping link",
                    (unsigned)s, (int)MAX_RETX);
                reset_unlocked(true);
                hw.unlock();
                hw.sendBreak();
                return;
            }
            retxCount_[s]++;
            sentAtMs_[s] = now;
            // Translate the timed-out chunk to its message's base
            // cobsSeq. The facade's cache is keyed by base — a
            // chunk seq lookup would miss and incorrectly drop the
            // link. baseSeq_[s] == s for keepalives / 1-chunk
            // messages; the cache handles those the same way.
            uint8_t base = baseSeq_[s];
            Log::getLog().warning(ALINK_TAG,
                "cobsSeq=%u (base=%u) ACK timeout (age=%lu ms, retx #%d) -> retransmit",
                (unsigned)s, (unsigned)base, (unsigned long)age, retxCount_[s]);
            // Delegate the actual resend to the facade (AutoLink).
            // It returns true to drop the link (e.g. cache miss).
            if (arqRetxCallback_) {
                if (arqRetxCallback_(base, arqCtx_)) {
                    reset_unlocked(true);
                    hw.unlock();
                    hw.sendBreak();
                    return;
                }
            } else {
                // No facade: protocol layer can't retransmit on its
                // own (no payload cache). Log once per link and let
                // MAX_RETX trip on the next few ticks.
                Log::getLog().warning(ALINK_TAG,
                    "cobsSeq=%u ACK timeout but no ARQ facade registered; "
                    "best-effort mode (will drop on MAX_RETX)", (unsigned)s);
            }
            retxNeeded_ = true;
            break;  // one per timer tick — the facade schedules more
        }
    }
    hw.startTimer(okTickMs());
}

int ALink::pendingAcks() const {
    // Read-only access; the map is updated under the lock from
    // onAck and onPayload. Not used in the hot path — for tests +
    // diagnostics only.
    int n = 0;
    for (int i = 0; i < 256; i++) if (ackedPending_[i]) n++;
    return n;
}

bool ALink::isAcked(uint8_t cobsSeq) const {
    return !ackedPending_[cobsSeq];
}

int ALink::popRetransmitSlot() {
    // Find a cobsSeq whose ACK has expired. Called from the
    // facade-level timer; the facade must hold hw.lock() because
    // we touch the same state.
    uint32_t now = hw.nowMs();
    for (int s = 0; s < 256; s++) {
        if (!ackedPending_[s]) continue;
        if (now - sentAtMs_[s] < ACK_RTO_MS) continue;
        // Pop: clear the slot so we don't return it again on the
        // next pop. The caller (facade) is responsible for either
        // resending (which will re-arm the slot) or giving up.
        uint8_t seq = (uint8_t)s;
        ackedPending_[seq] = false;
        retxCount_[seq]++;  // count this pop as a retransmit attempt
        sentAtMs_[seq] = now;
        return seq;
    }
    return -1;
}

void ALink::onTimerSwp_unlocked() {
    if (isMaster) {
        if (spdI >= (int)cfg.allowedBaudsCount) {
            // Past the last baud — should have transitioned to LCK
            // already, but if a re-sweep raced with the end of the
            // baud list, finish the transition now.
            changeState_unlocked(State::LCK);
            lckRetries = 0;
            spdI = 0;
            hw.setSpd(cfg.allowedBauds[0]);
            if (isMaster) hw.startTimer(cfg.delayMs);
            return;
        }
        if (pingSample == 0) {
            // DEBUG: fires on every baud index transition (~10 lines
            // per negotiation). The matching Pong "full sweep done"
            // stays at INFO (one line per sweep).
            Log::getLog().debug(ALINK_TAG, "SWP Ping baud[%d]=%lu",
                spdI, (unsigned long)cfg.allowedBauds[spdI]);
        }
        sendFrame_unlocked(PING_CMD);
        int samples = baudSweep.samplesPerBaud();
        if (samples < 1) samples = 1;
        if (pingSample + 1 >= samples) {
            pingSample = 0;
            spdI++;
            if (spdI < (int)cfg.allowedBaudsCount) {
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
    // when we wrap, fire WIRING CHECK if a full sweep heard nothing.
    int scored = baudSweep.scoreAt(spdI);
    int needed = baudSweep.minHitsForReliable();
    if (scored < needed) {
        Log::getLog().info(ALINK_TAG,
            "SWP Pong baud[%d]=%lu scored %d/%d (%d raw bytes rx), advancing",
            spdI,
            (unsigned long)(spdI < (int)cfg.allowedBaudsCount
                            ? cfg.allowedBauds[spdI] : 0),
            scored, needed, swpRxBytes);
        swpRxBytes = 0;
        spdI++;
        if (spdI >= (int)cfg.allowedBaudsCount) {
            bool anyPinged = false;
            for (int i = 0; i < (int)cfg.allowedBaudsCount; i++) {
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
    if (lckRetries > (int)cfg.allowedBaudsCount * 2) {
        // DEBUG: the visible signal that a re-sweep happened is the
        // preceding "Locked at N baud" INFO line.
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
