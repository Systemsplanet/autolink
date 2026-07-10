
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
    hw.tx(frame, (int)(encLen + 2));
}

void Link::sendCobsFrame_unlocked(const uint8_t *b, int n) {
    uint8_t seq = txSeq;
    buildAndTxCobsFrame_unlocked(seq, b, n);

    txSeq = (txSeq == COBS_SEQ_MAX) ? 0 : (uint8_t)(txSeq + 1);
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
}

void Link::sendNakFrame_unlocked(uint8_t missingCobsSeq) {
    sendCtrlCobsFrame_unlocked(NAK_TYPE, missingCobsSeq);
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
