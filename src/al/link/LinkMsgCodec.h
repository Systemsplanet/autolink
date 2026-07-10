// App-message layer: 6-byte header (len u32 LE + crc16 LE) over the
// in-order byte stream. Pure decisions here; buffer I/O stays in Link.
#pragma once
#include "al/link/LinkWire.h"
#include <stdint.h>
#include <stddef.h>

namespace autolink {

inline void msgHdrEncode(uint32_t len, uint16_t crc, uint8_t out[MSG_HDR]) {
    out[0] = (uint8_t)(len);
    out[1] = (uint8_t)(len >> 8);
    out[2] = (uint8_t)(len >> 16);
    out[3] = (uint8_t)(len >> 24);
    out[4] = (uint8_t)(crc);
    out[5] = (uint8_t)(crc >> 8);
}

struct MsgHdr {
    uint32_t len;
    uint16_t crc;
};

inline MsgHdr msgHdrDecode(const uint8_t h[MSG_HDR]) {
    MsgHdr m;
    m.len = (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) |
        ((uint32_t)h[3] << 24);
    m.crc = (uint16_t)h[4] | ((uint16_t)h[5] << 8);
    return m;
}

// First offset in b[0..n) holding a plausible header (len in bounds
// and crc16 over the following len bytes matches); -1 if none.
int msgResyncScan(const uint8_t *b, int n, size_t maxMsg);

// RX cursor: between-messages (-1) or inside a message of len_ bytes
// whose crc16 must match crc_.
class LinkMsgCodec {
public:
    bool inMsg() const { return len_ >= 0; }
    bool beginMsg(const uint8_t h[MSG_HDR], size_t maxMsg) {
        MsgHdr m = msgHdrDecode(h);
        if (m.len == 0 || m.len > maxMsg)
            return false;
        len_ = (int)m.len;
        crc_ = m.crc;
        return true;
    }
    int len() const { return len_; }
    uint16_t crc() const { return crc_; }
    void reset() { len_ = -1; }

private:
    int len_ = -1;
    uint16_t crc_ = 0;
};

} // namespace autolink
