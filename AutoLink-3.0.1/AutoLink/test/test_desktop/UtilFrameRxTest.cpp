// Host-only unit tests for UtilFrameRx. Arduino/ESP32 builds skip this file.
#ifndef ARDUINO

#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>
#include "UtilFrameRx.h"
#include "UtilCobs.h"
#include "UtilCrc.h"

using namespace autolink;

// Collects payloads and errors; can be told to report a link drop after a
// given number of errors (mimicking ALink's err threshold).
class MockListener : public UtilFrameRx::Listener {
public:
    std::vector<std::vector<uint8_t>> payloads;
    int errors = 0;
    int dropAfterErrors = -1;   // -1 = never drop
    bool onPayload(const uint8_t* b, int n) override {
        payloads.emplace_back(b, b + n);
        return false;
    }
    bool onFrameError() override {
        errors++;
        return dropAfterErrors >= 0 && errors >= dropAfterErrors;
    }
};

// Build one wire frame: 0x00 + COBS(payload + crc8) + 0x00.
static std::vector<uint8_t> wireFrame(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> raw = payload;
    raw.push_back(UtilCrc::crc8(payload.data(), (int)payload.size()));
    std::vector<uint8_t> enc(UtilCobs::encodedMax(raw.size()) + 2);
    size_t n = UtilCobs::encode(raw.data(), raw.size(), enc.data() + 1);
    enc[0] = 0x00;
    enc[1 + n] = 0x00;
    enc.resize(n + 2);
    return enc;
}

void test_single_frame() {
    std::cout << "\n=== Test: Single Frame Delivered ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    std::vector<uint8_t> p = {0x10, 0x00, 0x20, 0xFF};
    auto w = wireFrame(p);
    assert(rx.feed(w.data(), (int)w.size()) == (int)w.size());
    assert(lis.payloads.size() == 1 && lis.payloads[0] == p && lis.errors == 0);
    std::cout << "PASS" << std::endl;
}

// Frames must survive arriving one byte per feed() (UART events fragment).
void test_split_across_feeds() {
    std::cout << "\n=== Test: Frame Split Across Feeds ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    std::vector<uint8_t> p;
    for (int i = 0; i < 100; i++) p.push_back((uint8_t)(i + 1));
    auto w = wireFrame(p);
    for (uint8_t b : w) rx.feed(&b, 1);
    assert(lis.payloads.size() == 1 && lis.payloads[0] == p && lis.errors == 0);
    std::cout << "PASS" << std::endl;
}

void test_back_to_back_frames() {
    std::cout << "\n=== Test: Back-to-Back Frames in One Feed ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    std::vector<uint8_t> a = {1, 2, 3}, b = {9, 8, 7, 6, 5};
    auto w = wireFrame(a);
    auto wb = wireFrame(b);
    w.insert(w.end(), wb.begin(), wb.end());
    rx.feed(w.data(), (int)w.size());
    assert(lis.payloads.size() == 2);
    assert(lis.payloads[0] == a && lis.payloads[1] == b);
    std::cout << "PASS" << std::endl;
}

void test_bad_crc_is_error() {
    std::cout << "\n=== Test: Bad CRC Reports Error, Stream Stays Synced ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    std::vector<uint8_t> p = {0x11, 0x22, 0x33};
    auto bad = wireFrame(p);
    bad[1] ^= 0x01;   // corrupt inside the frame
    rx.feed(bad.data(), (int)bad.size());
    assert(lis.payloads.empty() && lis.errors == 1);
    // A clean frame right after must still be delivered.
    auto good = wireFrame(p);
    rx.feed(good.data(), (int)good.size());
    assert(lis.payloads.size() == 1 && lis.payloads[0] == p);
    std::cout << "PASS" << std::endl;
}

void test_malformed_and_crc_only_are_errors() {
    std::cout << "\n=== Test: Malformed COBS / CRC-Only Frames ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    uint8_t crc_only[] = {0x00, 0x02, 0xAB, 0x00};  // decodes to 1 byte
    rx.feed(crc_only, sizeof(crc_only));
    assert(lis.errors == 1);
    uint8_t malformed[] = {0x00, 0x09, 0x11, 0x00}; // code points past end
    rx.feed(malformed, sizeof(malformed));
    assert(lis.errors == 2);
    assert(lis.payloads.empty());
    std::cout << "PASS" << std::endl;
}

// 300 non-delimited bytes overflow the 256-byte accumulator -> one error,
// and the parser recovers for the next clean frame.
void test_oversize_frame() {
    std::cout << "\n=== Test: Oversize Frame Overflow ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    std::vector<uint8_t> flood(300, 0xCC);
    rx.feed(flood.data(), (int)flood.size());
    assert(lis.errors >= 1 && lis.payloads.empty());
    rx.reset();
    std::vector<uint8_t> p = {0x42};
    auto w = wireFrame(p);
    rx.feed(w.data(), (int)w.size());
    assert(lis.payloads.size() == 1 && lis.payloads[0] == p);
    std::cout << "PASS" << std::endl;
}

// Lone zeros between frames are the link keepalive: skipped, no error.
void test_keepalive_zeros_skipped() {
    std::cout << "\n=== Test: Stray Zeros (Keepalive) Skipped ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    uint8_t zeros[] = {0x00, 0x00, 0x00, 0x00};
    rx.feed(zeros, sizeof(zeros));
    assert(lis.payloads.empty() && lis.errors == 0);
    std::cout << "PASS" << std::endl;
}

// When the listener reports a link drop, feed() must stop consuming and
// return how far it got, leaving the rest for the caller's command parser.
void test_drop_stops_feed_early() {
    std::cout << "\n=== Test: Listener Drop Stops Feed Early ===" << std::endl;
    MockListener lis;
    lis.dropAfterErrors = 2;
    UtilFrameRx rx(lis);
    // Two CRC-only frames (errors), then trailing bytes that must NOT be eaten.
    std::vector<uint8_t> ev = {0x00, 0x02, 0xAB, 0x00, 0x02, 0xCD, 0x00,
                               0xAA, 0x55, 0x22, 0x99};
    int used = rx.feed(ev.data(), (int)ev.size());
    assert(lis.errors == 2);
    assert(used == 7);   // stopped right after the dropping delimiter
    std::cout << "PASS" << std::endl;
}

// reset() discards a partial frame so post-resweep garbage can't leak.
void test_reset_discards_partial() {
    std::cout << "\n=== Test: reset() Discards Partial Frame ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    uint8_t partial[] = {0x05, 0x11, 0x22};   // frame body, no delimiter yet
    rx.feed(partial, sizeof(partial));
    rx.reset();
    uint8_t delim = 0x00;                     // would complete the stale frame
    rx.feed(&delim, 1);
    assert(lis.payloads.empty() && lis.errors == 0);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running UtilFrameRx Tests ===" << std::endl;
    test_single_frame();
    test_split_across_feeds();
    test_back_to_back_frames();
    test_bad_crc_is_error();
    test_malformed_and_crc_only_are_errors();
    test_oversize_frame();
    test_keepalive_zeros_skipped();
    test_drop_stops_feed_early();
    test_reset_discards_partial();
    std::cout << "\n=== UtilFrameRx Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif // ARDUINO
