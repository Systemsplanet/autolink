#include "ALink.h"
#include "Log.h"
#include <algorithm>
#include <string.h>

static constexpr const char* ALINK_TAG = "AutoLink";

namespace autolink {

// Scratch buffers must hold one max chunk after COBS + CRC8 + delimiters.
static_assert(MAX_CHUNK + 5 <= 256, "MAX_CHUNK too large for 256-byte frame buffers");

static const uint8_t crc8_lut[256] = {
    0x00, 0x07, 0x0E, 0x09, 0x1C, 0x1B, 0x12, 0x15, 0x38, 0x3F, 0x36, 0x31, 0x24, 0x23, 0x2A, 0x2D,
    0x70, 0x77, 0x7E, 0x79, 0x6C, 0x6B, 0x62, 0x65, 0x48, 0x4F, 0x46, 0x41, 0x54, 0x53, 0x5A, 0x5D,
    0xE0, 0xE7, 0xEE, 0xE9, 0xFC, 0xFB, 0xF2, 0xF5, 0xD8, 0xDF, 0xD6, 0xD1, 0xC4, 0xC3, 0xCA, 0xCD,
    0x90, 0x97, 0x9E, 0x99, 0x8C, 0x8B, 0x82, 0x85, 0xA8, 0xAF, 0xA6, 0xA1, 0xB4, 0xB3, 0xBA, 0xBD,
    0xC7, 0xC0, 0xC9, 0xCE, 0xDB, 0xDC, 0xD5, 0xD2, 0xFF, 0xF8, 0xF1, 0xF6, 0xE3, 0xE4, 0xED, 0xEA,
    0xB7, 0xB0, 0xB9, 0xBE, 0xAB, 0xAC, 0xA5, 0xA2, 0x8F, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9D, 0x9A,
    0x27, 0x20, 0x29, 0x2E, 0x3B, 0x3C, 0x35, 0x32, 0x1F, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0D, 0x0A,
    0x57, 0x50, 0x59, 0x5E, 0x4B, 0x4C, 0x45, 0x42, 0x6F, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7D, 0x7A,
    0x89, 0x8E, 0x87, 0x80, 0x95, 0x92, 0x9B, 0x9C, 0xB1, 0xB6, 0xBF, 0xB8, 0xAD, 0xAA, 0xA3, 0xA4,
    0xF9, 0xFE, 0xF7, 0xF0, 0xE5, 0xE2, 0xEB, 0xEC, 0xC1, 0xC6, 0xCF, 0xC8, 0xDD, 0xDA, 0xD3, 0xD4,
    0x69, 0x6E, 0x67, 0x60, 0x75, 0x72, 0x7B, 0x7C, 0x51, 0x56, 0x5F, 0x58, 0x4D, 0x4A, 0x43, 0x44,
    0x19, 0x1E, 0x17, 0x10, 0x05, 0x02, 0x0B, 0x0C, 0x21, 0x26, 0x2F, 0x28, 0x3D, 0x3A, 0x33, 0x34,
    0x4E, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5C, 0x5B, 0x76, 0x71, 0x78, 0x7F, 0x6A, 0x6D, 0x64, 0x63,
    0x3E, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2C, 0x2B, 0x06, 0x01, 0x08, 0x0F, 0x1A, 0x1D, 0x14, 0x13,
    0xAE, 0xA9, 0xA0, 0xA7, 0xB2, 0xB5, 0xBC, 0xBB, 0x96, 0x91, 0x98, 0x9F, 0x8A, 0x8D, 0x84, 0x83,
    0xDE, 0xD9, 0xD0, 0xD7, 0xC2, 0xC5, 0xCC, 0xCB, 0xE6, 0xE1, 0xE8, 0xEF, 0xFA, 0xFD, 0xF4, 0xF3
};

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
      state(State::OK), errs(0), spdI(0), rxIdx(0), relRxIdx(0),
      rxMsgLen(-1), rxMsgCrc(0), txBytes(0), rxBytes(0)
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
        // sendBreak() delivers onBreak() (sync on MockHal, async via UART_BREAK on
        // EspHal). Do not call onBreak() here too — it would reset spdI after the
        // first timer tick.
        hw.sendBreak();
    } else {
        hw.lock();
        changeState_unlocked(State::SWP);
        spdI = 0; rxIdx = 0; relRxIdx = 0;
        rxMsgLen = -1;
        memset(relRxBuf, 0, sizeof(relRxBuf));
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

uint8_t ALink::calcCrc(const uint8_t* data, int len) const {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) crc = crc8_lut[crc ^ data[i]];
    return crc;
}

// CRC-16/CCITT-FALSE: end-to-end integrity for whole messages, independent of
// the per-frame CRC8. Catches errors a single 8-bit frame check could miss.
uint16_t ALink::calcCrc16(const uint8_t* data, int len) const {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

size_t ALink::cobsEncode(const uint8_t *ptr, size_t length, uint8_t *dst) const {
    size_t read_index = 0, write_index = 1, code_index = 0;
    uint8_t code = 1;
    while(read_index < length) {
        if(ptr[read_index] == 0) {
            dst[code_index] = code; code = 1;
            code_index = write_index++; read_index++;
        } else {
            dst[write_index++] = ptr[read_index++]; code++;
            if(code == 0xFF) { dst[code_index] = code; code = 1; code_index = write_index++; }
        }
    }
    dst[code_index] = code;
    return write_index;
}

size_t ALink::cobsDecode(const uint8_t *ptr, size_t length, uint8_t *dst) const {
    size_t read_index = 0, write_index = 0;
    uint8_t code, i;
    while(read_index < length) {
        code = ptr[read_index];
        if(read_index + code > length && code != 1) return 0;
        read_index++;
        for(i = 1; i < code; i++) dst[write_index++] = ptr[read_index++];
        if(code < 0xFF && read_index < length) dst[write_index++] = 0;
    }
    return write_index;
}

void ALink::sendFrame(uint8_t payload) {
    uint8_t frame[4] = {0xAA, 0x55, payload, 0};
    frame[3] = calcCrc(frame, 3);
    hw.lock();
    hw.tx(frame, 4);
    hw.flushTx();
    hw.unlock();
}

void ALink::err() {
    hw.lock();
    if (state != State::OK) { hw.unlock(); return; }
    errs++;
    bool trigger = (errs > cfg.errThreshold);
    if (trigger) Log::getLog().info(ALINK_TAG, "Error threshold exceeded (%d > %d). Dropping link.",
                                    errs, cfg.errThreshold);
    hw.unlock();
    if (trigger) hw.sendBreak();
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
        State s = state;
        if (s == State::OK) hw.tx(b, len);
        hw.unlock();
        if (s != State::OK) return 0;
        hw.lock(); txBytes += len; hw.unlock();
        return len;
    }

    int offset = 0;
    uint8_t unenc[MAX_CHUNK + 1];
    uint8_t frame[MAX_CHUNK + 5];
    while(offset < len) {
        hw.lock(); State s = state; hw.unlock();
        if (s != State::OK) break;

        int chunk = std::min(len - offset, MAX_CHUNK);
        memcpy(unenc, b + offset, chunk);
        unenc[chunk] = calcCrc(unenc, chunk);

        frame[0] = 0x00;
        size_t encLen = cobsEncode(unenc, chunk + 1, frame + 1);
        frame[1 + encLen] = 0x00;

        // Hold the lock across the whole frame so a concurrent writer cannot
        // interleave bytes. Each frame is <= 255 B, well under the TX FIFO.
        hw.lock();
        hw.tx(frame, encLen + 2);
        hw.unlock();

        offset += chunk;
    }
    hw.lock(); txBytes += offset; hw.unlock();
    return offset;
}

void ALink::flush() { hw.flushTx(); }

bool ALink::sendMsg(const uint8_t* b, int len) {
    if (len <= 0 || (size_t)len > cfg.maxMsg) return false;
    { hw.lock(); State s = state; hw.unlock(); if (s != State::OK) return false; }

    uint16_t c = calcCrc16(b, len);
    uint8_t hdr[MSG_HDR] = {
        (uint8_t)(len), (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24),
        (uint8_t)(c), (uint8_t)(c >> 8)
    };
    if (write(hdr, MSG_HDR) != MSG_HDR) return false;
    return write(b, len) == len;
}

int ALink::recvMsg(uint8_t* out, int max_len) {
    if (rxMsgLen < 0) {
        if (hw.appBufAvailable() < MSG_HDR) return 0;
        uint8_t h[MSG_HDR];
        readStream(h, MSG_HDR);
        uint32_t L = (uint32_t)h[0] | ((uint32_t)h[1] << 8) |
                     ((uint32_t)h[2] << 16) | ((uint32_t)h[3] << 24);
        rxMsgCrc = (uint16_t)h[4] | ((uint16_t)h[5] << 8);
        if (L == 0 || L > cfg.maxMsg) {
            // Garbage length: stream is desynced. Flush and signal an error so
            // the link can drop and re-negotiate.
            hw.clearAppBuf();
            rxMsgLen = -1;
            err();
            return -1;
        }
        rxMsgLen = (int)L;
    }

    if (hw.appBufAvailable() < rxMsgLen) return 0; // wait for the full payload

    int len = rxMsgLen;
    rxMsgLen = -1;

    if (len > max_len) {
        // Caller's buffer is too small: drain to stay in sync, report error.
        uint8_t sink[256];
        int left = len;
        while (left > 0) left -= readStream(sink, std::min(left, (int)sizeof(sink)));
        err();
        return -1;
    }

    readStream(out, len);
    if (calcCrc16(out, len) != rxMsgCrc) { err(); return -1; }
    return len;
}

void ALink::getStats(uint64_t& tx, uint64_t& rx) const {
    hw.lock(); tx = txBytes; rx = rxBytes; hw.unlock();
}
void ALink::resetStats() {
    hw.lock(); txBytes = 0; rxBytes = 0; hw.unlock();
}

State ALink::getState() const { hw.lock(); State s = state; hw.unlock(); return s; }
int ALink::getErrCount() const { hw.lock(); int e = errs; hw.unlock(); return e; }
int ALink::getCurrentSpdIndex() const { hw.lock(); int idx = spdI; hw.unlock(); return idx; }

void ALink::onRx(const uint8_t* data, int len) {
    int i = 0;
    while (i < len) {
        hw.lock(); State cur_state = state; hw.unlock();

        if (cur_state == State::OK) {
            if (cfg.reliableMode) {
                uint8_t decoded[MAX_CHUNK + 2];
                for (; i < len; i++) {
                    uint8_t b = data[i];
                    if (b == 0x00) {
                        if (relRxIdx > 0) {
                            size_t decLen = cobsDecode(relRxBuf, relRxIdx, decoded);
                            relRxIdx = 0;
                            if (decLen > 1) {
                                uint8_t crc = calcCrc(decoded, decLen - 1);
                                if (crc == decoded[decLen - 1]) {
                                    hw.pushAppBuf(decoded, decLen - 1);
                                    hw.lock(); rxBytes += (decLen - 1); hw.unlock();
                                } else { err(); break; }
                            } else if (decLen > 0) { err(); break; }
                        }
                    } else {
                        if (relRxIdx < (int)sizeof(relRxBuf)) {
                            relRxBuf[relRxIdx++] = b;
                        } else {
                            relRxIdx = 0;
                            memset(relRxBuf, 0, sizeof(relRxBuf));
                        }
                    }
                }
            } else {
                int n = len - i;
                hw.pushAppBuf(data + i, n);
                hw.lock(); rxBytes += n; hw.unlock();
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
                if (calcCrc(rxBuf, 3) == rxBuf[3]) {
                    uint8_t payload = rxBuf[2];
                    if (cur_state == State::SWP) {
                        if (!isMaster && payload == REQ_CMD) {
                            hw.lock();
                            int best = bestSpd_unlocked();
                            hw.unlock();
                            sendFrame(best);
                            hw.setSpd(cfg.allowedBauds[best]);
                            hw.lock();
                            errs = 0;
                            changeState_unlocked(State::OK);
                            hw.unlock();
                        } else if (payload == PING_CMD && spdI < (int)cfg.allowedBauds.size()) {
                            hw.lock();
                            scores[spdI]++;
                            spdI++;
                            // Retune in lockstep with the master so the next PING,
                            // sent at the next baud, can actually be decoded. This is
                            // what makes scoring (and auto-baud) work on real hardware.
                            int next = (spdI < (int)cfg.allowedBauds.size()) ? spdI : 0;
                            hw.unlock();
                            hw.setSpd(cfg.allowedBauds[next]); // baud[0] readies us for REQ
                        }
                    }
                    else if (cur_state == State::LCK) {
                        if (isMaster) {
                            if (payload < (int)cfg.allowedBauds.size()) {
                                hw.setSpd(cfg.allowedBauds[payload]);
                                hw.lock();
                                errs = 0;
                                changeState_unlocked(State::OK);
                                hw.unlock();
                            }
                        } else {
                            if (payload == REQ_CMD) {
                                hw.lock();
                                int best = bestSpd_unlocked();
                                hw.unlock();
                                sendFrame(best);
                                hw.setSpd(cfg.allowedBauds[best]);
                                hw.lock();
                                errs = 0;
                                changeState_unlocked(State::OK);
                                hw.unlock();
                            }
                        }
                    }
                }
            }
        }
    }
}

void ALink::onBreak() {
    hw.lock();
    changeState_unlocked(State::SWP);
    spdI = 0;
    rxIdx = 0;
    relRxIdx = 0;
    rxMsgLen = -1;
    memset(relRxBuf, 0, sizeof(relRxBuf));
    for(int i=0; i<(int)scores.size(); i++) scores[i]=0;
    hw.unlock();

    hw.clearAppBuf();
    hw.setSpd(cfg.allowedBauds[0]);
    if (isMaster) hw.startTimer(cfg.delayMs);
}

void ALink::onTimer() {
    hw.lock();
    State s = state;
    int curSpd = spdI;
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
            hw.unlock();
            hw.setSpd(cfg.allowedBauds[0]);
            if (isMaster) hw.startTimer(cfg.delayMs);
        }
    }
    else if (s == State::LCK && isMaster) {
        sendFrame(REQ_CMD);
    }
}

} // namespace autolink
