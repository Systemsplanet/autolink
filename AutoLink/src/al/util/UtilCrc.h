#pragma once
#include <stdint.h>

// ----------------------------------------------------------------------------
// UtilCrc — table-driven CRC checksums used across the AutoLink stack.
//
// CRC-8 (poly 0x07, init 0x00) guards individual wire frames; CRC-16/
// CCITT-FALSE (poly 0x1021, init 0xFFFF) guards whole messages end-to-end.
// Stateless, allocation-free; the 768 bytes of LUT live in flash on ESP32.
// ----------------------------------------------------------------------------

namespace autolink {

class UtilCrc {
public:
    static uint8_t  crc8(const uint8_t* data, int len);
    static uint16_t crc16(const uint8_t* data, int len);

private:
    static const uint8_t  crc8_lut[256];
    static const uint16_t crc16_lut[256];
};

} // namespace autolink
