// Host-only unit tests for UtilCobs. Arduino/ESP32 builds skip this file.
#ifndef ARDUINO

#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>
#include "al/util/UtilCobs.h"

using namespace autolink;

static void roundtrip(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> enc(UtilCobs::encodedMax(in.size()) + 8, 0xEE);
    std::vector<uint8_t> dec(in.size() + 8, 0xEE);
    size_t encLen = UtilCobs::encode(in.data(), in.size(), enc.data());
    assert(encLen <= UtilCobs::encodedMax(in.size()));
    for (size_t i = 0; i < encLen; i++) assert(enc[i] != 0x00); // delimiter-free
    size_t decLen = UtilCobs::decode(enc.data(), encLen, dec.data());
    assert(decLen == in.size());
    assert(memcmp(dec.data(), in.data(), in.size()) == 0);
}

void test_roundtrip_patterns() {
    std::cout << "\n=== Test: Encode/Decode Round Trips ===" << std::endl;
    roundtrip({});                                  // empty
    roundtrip({0x00});                              // single zero
    roundtrip({0x42});                              // single non-zero
    roundtrip({0x00, 0x00, 0x00, 0x00});            // all zeros
    roundtrip({0x11, 0x22, 0x00, 0x33, 0x00});      // mixed
    std::vector<uint8_t> nz;                        // long, no zeros
    for (int i = 0; i < 1000; i++) nz.push_back((uint8_t)((i % 255) + 1));
    roundtrip(nz);
    std::vector<uint8_t> alt;                       // alternating zeros
    for (int i = 0; i < 1000; i++) alt.push_back((uint8_t)(i & 1));
    roundtrip(alt);
    std::cout << "PASS" << std::endl;
}

// 254 non-zero bytes is the 0xFF code-group boundary; 253/254/255/508
// straddle it. An off-by-one here corrupts every long frame.
void test_code_group_boundary() {
    std::cout << "\n=== Test: 0xFF Code-Group Boundary ===" << std::endl;
    for (int n : {253, 254, 255, 507, 508, 509}) {
        std::vector<uint8_t> in;
        for (int i = 0; i < n; i++) in.push_back((uint8_t)((i % 254) + 1));
        roundtrip(in);
    }
    // Zero right after a full group.
    std::vector<uint8_t> in;
    for (int i = 0; i < 254; i++) in.push_back(0x55);
    in.push_back(0x00);
    in.push_back(0x77);
    roundtrip(in);
    std::cout << "PASS" << std::endl;
}

void test_malformed_decode_rejected() {
    std::cout << "\n=== Test: Malformed Input Rejected ===" << std::endl;
    uint8_t out[64];
    uint8_t zero_code[] = {0x00, 0x11};          // zero code byte is illegal
    assert(UtilCobs::decode(zero_code, 2, out) == 0);
    uint8_t truncated[] = {0x05, 0x11, 0x22};    // code points past the end
    assert(UtilCobs::decode(truncated, 3, out) == 0);
    uint8_t bare[] = {0xFF};                     // full group with no bytes
    assert(UtilCobs::decode(bare, 1, out) == 0);
    std::cout << "PASS" << std::endl;
}

void test_encoded_max_bound() {
    std::cout << "\n=== Test: encodedMax() Is a True Upper Bound ===" << std::endl;
    for (int n : {0, 1, 2, 253, 254, 255, 508, 1000}) {
        // Worst case for COBS overhead is a zero-free payload.
        std::vector<uint8_t> in(n, 0x5A);
        std::vector<uint8_t> enc(UtilCobs::encodedMax(n) + 1, 0);
        size_t encLen = UtilCobs::encode(in.data(), in.size(), enc.data());
        assert(encLen <= UtilCobs::encodedMax(n));
    }
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running UtilCobs Tests ===" << std::endl;
    test_roundtrip_patterns();
    test_code_group_boundary();
    test_malformed_decode_rejected();
    test_encoded_max_bound();
    std::cout << "\n=== UtilCobs Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif // ARDUINO
