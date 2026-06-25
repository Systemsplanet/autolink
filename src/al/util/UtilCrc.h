// CRC-8 (per-frame integrity) and CRC-16 (per-message
// integrity) with lookup tables. crc8 covers the
// 5-byte control frame and the ACK/NAK envelope; crc16
// covers the assembled MSG_HDR + payload.
#pragma once
#include <stdint.h>

namespace autolink
{
class UtilCrc
{
public:
    static uint8_t crc8(const uint8_t *data, int len);
    static uint16_t crc16(const uint8_t *data,
                          int len);

private:
    static const uint8_t crc8_lut[256];
    static const uint16_t crc16_lut[256];
};

} // namespace autolink
