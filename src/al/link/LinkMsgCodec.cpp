#include "al/link/LinkMsgCodec.h"
#include "al/util/UtilCrc.h"

namespace autolink {

int msgResyncScan(const uint8_t *b, int n, size_t maxMsg) {
    for (int d = 0; d + MSG_HDR <= n; d++) {
        MsgHdr m = msgHdrDecode(b + d);
        if (m.len < 1 || m.len > maxMsg)
            continue;
        if (d + (int)m.len + MSG_HDR > n)
            continue;
        if (UtilCrc::crc16(b + d + MSG_HDR, (int)m.len) != m.crc)
            continue;
        return d;
    }
    return -1;
}

} // namespace autolink
