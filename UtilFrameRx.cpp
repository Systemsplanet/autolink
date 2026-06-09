#include "UtilFrameRx.h"
#include "UtilCobs.h"
#include "UtilCrc.h"
#include <string.h>

namespace autolink {

int UtilFrameRx::feed(const uint8_t* data, int len) {
    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b == 0x00) {
            if (idx == 0) continue;   // stray zero between frames (keepalive)
            size_t decLen = UtilCobs::decode(buf, idx, decoded);
            idx = 0;
            bool dropped;
            if (decLen > 1 &&
                UtilCrc::crc8(decoded, (int)decLen - 1) == decoded[decLen - 1]) {
                dropped = lis.onPayload(decoded, (int)decLen - 1);
            } else {
                // Malformed COBS, CRC-only frame, or bad CRC: all desync signals.
                dropped = lis.onFrameError();
            }
            if (dropped) return i + 1;
        } else if (idx < (int)sizeof(buf)) {
            buf[idx++] = b;
        } else {
            // Oversize frame: reset and report so persistence trips the
            // caller's error threshold.
            idx = 0;
            if (lis.onFrameError()) return i + 1;
        }
    }
    return len;
}

void UtilFrameRx::reset() {
    idx = 0;
    memset(buf, 0, sizeof(buf));
}

} // namespace autolink
