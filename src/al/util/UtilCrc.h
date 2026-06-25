// CRC-8 (control/ACK frames) and
// CRC-16 (MSG_HDR + payload) with LUTs.
#pragma once
#include <stdint.h>

namespace autolink
{
class UtilCrc
{
public:
    static uint8_t crc8(const uint8_t *data, int len);
    static uint16_t crc16(const uint8_t *data, int len);

private:
    static const uint8_t crc8_lut[256];
    static const uint16_t crc16_lut[256];
};

} // namespace autolink