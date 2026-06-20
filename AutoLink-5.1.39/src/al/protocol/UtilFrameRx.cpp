// UtilFrameRx.cpp — reliable-mode frame receive accumulator.
// See UtilFrameRx.h for the public interface.
#include "al/protocol/UtilFrameRx.h"
#include "al/util/UtilCobs.h"
#include "al/util/UtilCrc.h"
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
            if (decLen >= 2 &&
                UtilCrc::crc8(decoded, (int)decLen - 1) == decoded[decLen - 1]) {
                // CRC valid. First byte discriminates data vs ACK.
                if (decoded[0] == ACK_TYPE) {
                    // ACK frame: second byte is the cobsSeq being acked.
                    // decLen >= 2 means we have at least ACK_TYPE +
                    // ackedSeq + CRC. The ackedSeq itself is
                    // unconstrained (any 0..255), so we don't validate
                    // its range — the upper layer (ALink) checks if it
                    // matches a pending slot.
                    dropped = lis.onAck(decoded[1]);
                } else {
                    // Data frame: first byte is cobsSeq; rest is payload.
                    uint8_t cobsSeq = decoded[0];
                    const uint8_t* payload = decoded + 1;
                    int payloadLen = (int)decLen - 1 - 1;  // -1 for CRC, -1 for cobsSeq
                    if (payloadLen < 0) payloadLen = 0;
                    dropped = lis.onPayload(cobsSeq, payload, payloadLen);
                }
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
