// CRC-8/CRC-16 known-answer tests.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <cstring>
#    include "al/util/UtilCrc.h"

using namespace autolink;

static uint8_t ref_crc8(const uint8_t *d, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= d[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07)
                               : (uint8_t)(crc << 1);
    }
    return crc;
}
static uint16_t ref_crc16(const uint8_t *d, int len)
{
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int k = 0; k < 8; k++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
    }
    return crc;
}

void test_known_answers()
{
    std::cout << "\n=== Test: CRC Known-Answer Vectors ===" << std::endl;
    const uint8_t check[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    assert(UtilCrc::crc8(check, 9) == 0xF4);
    assert(UtilCrc::crc16(check, 9) == 0x29B1);
    std::cout << "PASS" << std::endl;
}

void test_edges()
{
    std::cout << "\n=== Test: CRC Edge Cases ===" << std::endl;
    uint8_t b = 0x00;
    assert(UtilCrc::crc8(&b, 0) == 0x00);
    assert(UtilCrc::crc16(&b, 0) == 0xFFFF);
    assert(UtilCrc::crc8(&b, 1) == ref_crc8(&b, 1));
    b = 0xFF;
    assert(UtilCrc::crc8(&b, 1) == ref_crc8(&b, 1));
    assert(UtilCrc::crc16(&b, 1) == ref_crc16(&b, 1));
    std::cout << "PASS" << std::endl;
}

void test_lut_vs_reference()
{
    std::cout << "\n=== Test: LUT Agrees With Bitwise Reference ==="
              << std::endl;
    for (int v = 0; v < 256; v++) {
        uint8_t b = (uint8_t)v;
        assert(UtilCrc::crc8(&b, 1) == ref_crc8(&b, 1));
        assert(UtilCrc::crc16(&b, 1) == ref_crc16(&b, 1));
    }
    uint8_t buf[4096];
    uint32_t x = 0x12345678;
    for (int i = 0; i < (int)sizeof(buf); i++) {
        x = x * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(x >> 24);
    }
    assert(UtilCrc::crc8(buf, sizeof(buf)) == ref_crc8(buf, sizeof(buf)));
    assert(UtilCrc::crc16(buf, sizeof(buf)) == ref_crc16(buf, sizeof(buf)));
    std::cout << "PASS" << std::endl;
}

void test_error_detection()
{
    std::cout << "\n=== Test: Single-Bit Errors Detected ===" << std::endl;
    uint8_t buf[64];
    for (int i = 0; i < 64; i++)
        buf[i] = (uint8_t)(i * 7 + 3);
    uint8_t c8 = UtilCrc::crc8(buf, 64);
    uint16_t c16 = UtilCrc::crc16(buf, 64);
    for (int i = 0; i < 64; i++) {
        for (int bit = 0; bit < 8; bit++) {
            buf[i] ^= (uint8_t)(1 << bit);
            assert(UtilCrc::crc8(buf, 64) != c8);
            assert(UtilCrc::crc16(buf, 64) != c16);
            buf[i] ^= (uint8_t)(1 << bit);
        }
    }
    std::cout << "PASS" << std::endl;
}

int main()
{
    std::cout << "=== Running UtilCrc Tests ===" << std::endl;
    test_known_answers();
    test_edges();
    test_lut_vs_reference();
    test_error_detection();
    std::cout << "\n=== UtilCrc Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif