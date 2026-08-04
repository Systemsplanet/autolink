
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

void Link::sendPongAck_unlocked() { sendSweepFrame_unlocked(PONG_CMD); }

void Link::sendFrame_unlocked(uint8_t payload) {
    uint8_t frame[CTRL_FRAME_SIZE];
    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[CTRL_FRAME_SEQ_IDX] = txSeq;
    frame[CTRL_FRAME_PAYLOAD_IDX] = payload;
    frame[CTRL_FRAME_CRC_IDX] = UtilCrc::crc8(frame, CTRL_FRAME_SIZE - 1);
    if (hw.tx(frame, CTRL_FRAME_SIZE) != CTRL_FRAME_SIZE)
        Log::log().error(TAG, "sendFrame truncated");
    else
        Log::log().debug(TAG, "wire CTRL seq=%u payload=0x%02X",
                         (unsigned)txSeq, (unsigned)payload);

    txBytes += CTRL_FRAME_SIZE;
    lastTxMs = hw.nowMs();
}

// Sweep-frame send: identical to sendFrame_unlocked except the seq
// byte carries sweepEpoch_ instead of txSeq. handleSwp_unlocked
// already (void)ed the seq byte, so this reuses the slot to convey
// a session epoch that lets a still-OK peer detect a peer that
// restarted underneath it (the GAP...dropped storm trigger).
// Wire-format change: zero — the 5-byte control frame shape, the
// preamble, the payload byte, and the CRC are byte-for-byte
// identical to sendFrame_unlocked; only the meaning of byte[2]
// changes (epoch vs data seq) during the sweep phase.
void Link::sendSweepFrame_unlocked(uint8_t payload) {
    uint8_t frame[CTRL_FRAME_SIZE];
    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[CTRL_FRAME_SEQ_IDX] = sweepEpoch_;
    frame[CTRL_FRAME_PAYLOAD_IDX] = payload;
    frame[CTRL_FRAME_CRC_IDX] = UtilCrc::crc8(frame, CTRL_FRAME_SIZE - 1);
    if (hw.tx(frame, CTRL_FRAME_SIZE) != CTRL_FRAME_SIZE)
        Log::log().error(TAG, "sendSweepFrame truncated");
    else
        Log::log().debug(TAG, "wire SWEEP epoch=%u payload=0x%02X",
                         (unsigned)sweepEpoch_, (unsigned)payload);

    txBytes += CTRL_FRAME_SIZE;
    lastTxMs = hw.nowMs();
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
    int frameBytes = (int)(encLen + 2);
    hw.tx(frame, frameBytes);
}

void Link::sendCobsFrame_unlocked(const uint8_t *b, int n) {
    uint8_t seq = txSeq;
    buildAndTxCobsFrame_unlocked(seq, b, n);

    txSeq = (txSeq == COBS_SEQ_MAX) ? 0 : (uint8_t)(txSeq + 1);
    // COBS chunk sent: per-async-pipeline rate would flood
    // (MAX_TX_PER_LOOP per loop = hundreds/sec at ASYNC). Verbose
    // captures the seq + bytes for deep-trace only; the
    // SYNC/ASYNC distinction plus the wire-recvd companion in
    // Ping already gives the operator the per-chunk shape
    // without lifting to info. Per-frame trace;
    // default-compiled-out (see onPayload's field
    // comment for the the prior shape transport-saturation
    // rationale). Pinned by WireTraceOffByDefaultTest.
#ifdef AUTOLINK_TRACE_WIRE
    Log::log().verbose(TAG, "wire COBS seq=%u n=%d", (unsigned)seq, n);
#endif
}

uint8_t Link::sendCobsFrameAcked_unlocked(const uint8_t *b, int n,
                                          uint8_t baseSeq) {
    uint8_t seq = txSeq;
    sendCobsFrame_unlocked(b, n);
    if (cfg.mode != AutoLinkConfig::Mode::SYNC && !arq_.gbnActive()) {
        arq_.setGbnBase(seq);
        gbnAttempts_ = 0;
        gbnBackoffMs_ = 0;
        gbnLastRetxBase_ = 0xFF;
        consecutiveKeep_ = 0;
        arq_.setGbnActive(true);
    }
    arq_.onSent(seq, baseSeq, hw.nowMs());
    if (n > 0)
        arqCache_.insert(seq, b, n);
    return seq;
}

void Link::resendCobsFrame_unlocked(uint8_t seq, const uint8_t *b, int n) {
    buildAndTxCobsFrame_unlocked(seq, b, n);

    if (n > 0)
        txBytes += (uint64_t)n;
    lastTxMs = hw.nowMs();
}

// CRC8-tail + COBS-encode + tx a small control payload, counting the
// wire bytes (ACK/NAK traffic dominates Pong's tx rate).
void Link::txSmallCobs_unlocked(uint8_t *u, size_t rawLen) {
    u[rawLen - 1] = UtilCrc::crc8(u, rawLen - 1);
    uint8_t frame[16];
    frame[0] = 0x00;
    size_t el = UtilCobs::encode(u, rawLen, frame + 1);
    frame[1 + el] = 0x00;
    hw.tx(frame, (int)(el + 2));
    txBytes += (uint64_t)(el + 2);
    lastTxMs = hw.nowMs();
}

void Link::sendCtrlCobsFrame_unlocked(uint8_t type, uint8_t seq) {
    uint8_t u[3] = { type, seq, 0 };
    txSmallCobs_unlocked(u, 3);
}

void Link::sendAckFrame_unlocked(uint8_t ackedCobsSeq, uint16_t bytesRecvd) {
    uint8_t u[5] = { ACK_TYPE, ackedCobsSeq, (uint8_t)(bytesRecvd & 0xFF),
                     (uint8_t)((bytesRecvd >> 8) & 0xFF), 0 };
    txSmallCobs_unlocked(u, 5);
    // Pong's primary TX path: per-chunk-ack rate is ASYNC
    // pipeline rate. Verbose, not info, for the same reason as
    // sendCobsFrame_unlocked above. Per-frame trace;
    // default-compiled-out (see onPayload's field
    // comment). Pinned by WireTraceOffByDefaultTest.
#ifdef AUTOLINK_TRACE_WIRE
    Log::log().verbose(TAG, "wire ACK seq=%u bytesRecvd=%u",
                       (unsigned)ackedCobsSeq, (unsigned)bytesRecvd);
#endif
}

void Link::sendNakFrame_unlocked(uint8_t missingCobsSeq) {
    sendCtrlCobsFrame_unlocked(NAK_TYPE, missingCobsSeq);
    // NAK on the wire: per-async-pipeline rate (one per
    // out-of-order arrival). Debug for the same reason as
    // sendAckFrame_unlocked.
    Log::log().debug(TAG, "wire NAK missing=%u", (unsigned)missingCobsSeq);
}

bool Link::buildAndSendMsg_unlocked(const uint8_t *b, int len,
                                    uint8_t *outLastSeq) {
    uint8_t hdr[MSG_HDR];
    msgHdrEncode((uint32_t)len, UtilCrc::crc16(b, len), hdr);
    uint8_t lastSeq = 0;
    if (len + MSG_HDR <= MAX_CHUNK) {
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
