// UtilFrameRx.cpp — reliable-mode frame receive accumulator.
// See UtilFrameRx.h for the public interface.
#include "UtilFrameRx.h"
#include "UtilCobs.h"
#include "UtilCrc.h"
#include <string.h>

namespace autolink {

// Keepalive atom: 0x00 0x00. COBS bytes are 0x01..0xFF, so a real frame
// never produces two back-to-back zeros -- the pair is unambiguously a
// keepalive. Self-resyncing: at a clean boundary both bytes are skipped
// with no callback; mid-COBS the first 0x00 closes the partial (one
// onFrameError, correct: that partial was garbage) and the second is
// the keepalive start.
int UtilFrameRx::feed(const uint8_t* data, int len) {
    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b == 0x00) {
            if (idx == 0) {
                // Clean boundary. Consume a 0x00 0x00 atom if present,
                // else skip the lone zero.
                if (i + 1 < len && data[i + 1] == 0x00) i++;
                continue;
            }
            size_t decLen = UtilCobs::decode(buf, idx, decoded);
            idx = 0;
            bool dropped;
            if (decLen > 1 &&
                UtilCrc::crc8(decoded, (int)decLen - 1) == decoded[decLen - 1]) {
                dropped = lis.onPayload(decoded, (int)decLen - 1);
            } else {
                // Malformed COBS, CRC-only frame, or bad CRC: desync.
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
