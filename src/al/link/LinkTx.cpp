// LinkTx -- sendFrame, buildAndTxCobs, sendCobsFrame*, resendCobsFrame,
// sendCtrlCobsFrame (ACK / NAK), buildAndSendMsg_unlocked.
// sendPongAck_unlocked is the wire-side helper called from
// handleSwp_unlocked (LinkSweep.cpp) and processCtrlFrame_unlocked
// (LinkRx.cpp). It's a one-liner so it lives here next to
// sendFrame_unlocked.
//
// All these methods write to the wire. They take the link's
// mutex at the call site (sendMsg_unlocked is locked by the
// public sendMsg path; the SWP / RX paths already hold the
// lock when they call into here).
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/link/arq/LinkArq.h"
#include "al/util/Log.h"
#include "al/util/UtilCobs.h"
#include "al/util/UtilCrc.h"
#include <algorithm>
#include <cstring>

static constexpr const char *TAG = "AutoLink";

namespace autolink {
static_assert(
    MAX_CHUNK + MSG_HDR <= ArqCache::POOL_BUF_MAX,
    "MAX_CHUNK + MSG_HDR > ArqCache::POOL_BUF_MAX: bump POOL_BUF_MAX or shrink MAX_CHUNK");

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
    uint8_t frame[MAX_CHUNK + MSG_HDR];
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

void Link::sendAckFrame_unlocked(uint8_t ackedCobsSeq, uint16_t bytesRecvd) {
    // 5-byte raw ACK frame:
    //   [0xFF, seq, bytes_lo, bytes_hi, frame_crc8]
    // frame_crc8 covers bytes 0..3. The receiver's
    // onAck listener picks up bytesRecvd so Ping's
    // "echo <seq> <bytes> <pending>" log line shows
    // the actual payload length, not just the seq.
    //
    // Inline the encode rather than routing through
    // sendCtrlCobsFrame_unlocked() because that
    // helper produces a 3-byte payload and the new
    // ACK is 5 bytes. The two encodings share the
    // [type, seq, ..., crc8] structure but the body
    // length and CRC coverage differ; a parameterized
    // helper would need a payload-length + per-byte
    // CRC table, which costs more on the hot path
    // than the 6 extra bytes of frame-buffer code.
    uint8_t u[5] = {
        ACK_TYPE,
        ackedCobsSeq,
        (uint8_t)(bytesRecvd & 0xFF),
        (uint8_t)((bytesRecvd >> 8) & 0xFF),
        0
    };
    u[4] = UtilCrc::crc8(u, 4);
    uint8_t frame[16];
    frame[0] = 0x00;
    size_t el = UtilCobs::encode(u, 5, frame + 1);
    frame[1 + el] = 0x00;
    hw.tx(frame, (int)(el + 2));
}

void Link::sendNakFrame_unlocked(uint8_t missingCobsSeq) {
    sendCtrlCobsFrame_unlocked(NAK_TYPE, missingCobsSeq);
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

} // namespace autolink
