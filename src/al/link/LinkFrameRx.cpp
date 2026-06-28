// COBS frame decoder implementation.
#include "al/link/LinkFrameRx.h"
#include "al/util/UtilCobs.h"
#include "al/util/UtilCrc.h"
#include <string.h>

namespace autolink {
int UtilFrameRx::feed(const uint8_t *data, int len) {
    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b == 0x00) {
            if (idx == 0) {
                // Skip double-zero delimiter.
                if (i + 1 < len && data[i + 1] == 0x00)
                    i++;
                continue;
            }
            size_t decLen = UtilCobs::decode(buf, idx, decoded);
            idx = 0;
            bool dropped;
            // Wire ACK is 5 bytes raw:
            //   [0xFF, seq, bytes_lo, bytes_hi, frame_crc8]
            // Wire NAK is 3 bytes raw:
            //   [0xFE, seq, frame_crc8]
            // Data is at least 2 bytes raw (seq + crc8).
            if (decLen >= 5 && decoded[0] == ACK_TYPE &&
                UtilCrc::crc8(decoded, 4) == decoded[4]) {
                uint16_t bytesRecvd =
                    (uint16_t)decoded[2] | ((uint16_t)decoded[3] << 8);
                dropped = lis.onAck(decoded[1], bytesRecvd);
            } else if (decLen >= 3 && decoded[0] == ACK_TYPE &&
                       UtilCrc::crc8(decoded, 2) == decoded[2]) {
                // Legacy 3-byte ACK frame (no bytes-recvd):
                // a peer still running the 5.3.x wire format.
                // Decode it as bytes-recvd=0 so the link
                // layer can still ACK the slot. Future
                // revisions can drop this fallback once the
                // 5.4.x wire is universal.
                dropped = lis.onAck(decoded[1], 0);
            } else if (decLen >= 3 && decoded[0] == NAK_TYPE &&
                       UtilCrc::crc8(decoded, 2) == decoded[2]) {
                dropped = lis.onNak(decoded[1]);
            } else if (decLen >= 2 &&
                       UtilCrc::crc8(decoded, (int)decLen - 1) ==
                           decoded[decLen - 1]) {
                uint8_t seq = decoded[0];
                const uint8_t *pl = decoded + 1;
                int plen = (int)decLen - 2;
                if (plen < 0)
                    plen = 0;
                dropped = lis.onPayload(seq, pl, plen);
            } else {
                dropped = lis.onFrameError();
            }
            if (dropped)
                return i + 1;
        } else if (idx < (int)sizeof(buf)) {
            buf[idx++] = b;
        } else {
            idx = 0;
            if (lis.onFrameError())
                return i + 1;
        }
    }
    return len;
}

void UtilFrameRx::reset() {
    idx = 0;
    memset(buf, 0, sizeof(buf));
}

} // namespace autolink
