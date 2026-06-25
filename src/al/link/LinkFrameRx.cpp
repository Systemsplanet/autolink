// COBS frame decoder implementation.
#include "al/link/LinkFrameRx.h"
#include "al/util/UtilCobs.h"
#include "al/util/UtilCrc.h"
#include <string.h>

namespace autolink
{
int UtilFrameRx::feed(const uint8_t *data, int len)
{
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
            if (decLen >= 2 &&
                UtilCrc::crc8(decoded, (int)decLen - 1) ==
                    decoded[decLen - 1]) {
                if (decoded[0] == ACK_TYPE) {
                    dropped = lis.onAck(decoded[1]);
                } else if (decoded[0] == NAK_TYPE) {
                    dropped = lis.onNak(decoded[1]);
                } else {
                    uint8_t seq = decoded[0];
                    const uint8_t *pl = decoded + 1;
                    int plen = (int)decLen - 2;
                    if (plen < 0)
                        plen = 0;
                    dropped = lis.onPayload(seq, pl, plen);
                }
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

void UtilFrameRx::reset()
{
    idx = 0;
    memset(buf, 0, sizeof(buf));
}

} // namespace autolink
