// UtilFrameRx COBS decode + frame routing.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <vector>
#    include <cstring>
#    include "al/link/LinkFrameRx.h"
#    include "al/util/UtilCobs.h"
#    include "al/util/UtilCrc.h"

using namespace autolink;

class MockListener : public UtilFrameRx::Listener
{
public:
    std::vector<std::vector<uint8_t>> payloads;
    std::vector<uint8_t> seqs;
    std::vector<uint8_t> acks;
    std::vector<uint8_t> naks;
    int errors = 0;
    int dropAfterErrors = -1;
    bool onPayload(uint8_t cobsSeq, const uint8_t *b, int n) override
    {
        seqs.push_back(cobsSeq);
        payloads.emplace_back(b, b + n);
        return false;
    }
    bool onAck(uint8_t ackedCobsSeq) override
    {
        acks.push_back(ackedCobsSeq);
        return false;
    }
    bool onNak(uint8_t missingCobsSeq) override
    {
        naks.push_back(missingCobsSeq);
        return false;
    }
    bool onFrameError() override
    {
        errors++;
        return dropAfterErrors >= 0 && errors >= dropAfterErrors;
    }
};

static std::vector<uint8_t> wireFrame(const std::vector<uint8_t> &payload,
                                      uint8_t cobsSeq = 0)
{
    std::vector<uint8_t> raw;
    raw.push_back(cobsSeq);
    raw.insert(raw.end(), payload.begin(), payload.end());
    raw.push_back(UtilCrc::crc8(raw.data(), (int)raw.size()));
    std::vector<uint8_t> enc(UtilCobs::encodedMax(raw.size()) + 2);
    size_t n = UtilCobs::encode(raw.data(), raw.size(), enc.data() + 1);
    enc[0] = 0x00;
    enc[1 + n] = 0x00;
    enc.resize(n + 2);
    return enc;
}

void test_single_frame()
{
    std::cout << "\n=== Test: Single Frame Delivered with cobsSeq ==="
              << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    std::vector<uint8_t> p = { 0x10, 0x00, 0x20, 0xFF };
    auto w = wireFrame(p, 42);
    assert(rx.feed(w.data(), (int)w.size()) == (int)w.size());
    assert(lis.payloads.size() == 1 && lis.payloads[0] == p);
    assert(lis.seqs.size() == 1 && lis.seqs[0] == 42);
    assert(lis.errors == 0);
    std::cout << "PASS" << std::endl;
}

void test_split_across_feeds()
{
    std::cout << "\n=== Test: Frame Split Across Feeds ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    std::vector<uint8_t> p;
    for (int i = 0; i < 100; i++)
        p.push_back((uint8_t)(i + 1));
    auto w = wireFrame(p, 7);
    for (uint8_t b : w)
        rx.feed(&b, 1);
    assert(lis.payloads.size() == 1 && lis.payloads[0] == p);
    assert(lis.seqs[0] == 7);
    assert(lis.errors == 0);
    std::cout << "PASS" << std::endl;
}

void test_back_to_back_frames()
{
    std::cout << "\n=== Test: Back-to-Back Frames in One Feed ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    std::vector<uint8_t> a = { 1, 2, 3 }, b = { 9, 8, 7, 6, 5 };
    auto w = wireFrame(a, 1);
    auto wb = wireFrame(b, 2);
    w.insert(w.end(), wb.begin(), wb.end());
    rx.feed(w.data(), (int)w.size());
    assert(lis.payloads.size() == 2);
    assert(lis.payloads[0] == a && lis.payloads[1] == b);
    assert(lis.seqs[0] == 1 && lis.seqs[1] == 2);
    std::cout << "PASS" << std::endl;
}

void test_bad_crc_is_error()
{
    std::cout << "\n=== Test: Bad CRC Reports Error, Stream Stays Synced ==="
              << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    std::vector<uint8_t> p = { 0x11, 0x22, 0x33 };
    auto bad = wireFrame(p, 1);
    bad[1] ^= 0x01;
    rx.feed(bad.data(), (int)bad.size());
    assert(lis.payloads.empty() && lis.errors == 1);
    auto good = wireFrame(p, 2);
    rx.feed(good.data(), (int)good.size());
    assert(lis.payloads.size() == 1 && lis.payloads[0] == p);
    assert(lis.seqs[0] == 2);
    std::cout << "PASS" << std::endl;
}

void test_malformed_and_crc_only_are_errors()
{
    std::cout << "\n=== Test: Malformed COBS / CRC-Only Frames ==="
              << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);

    uint8_t crc_only[] = { 0x00, 0x02, 0xAB, 0x00 };
    rx.feed(crc_only, sizeof(crc_only));
    assert(lis.errors == 1);
    uint8_t malformed[] = { 0x00, 0x09, 0x11, 0x00 };
    rx.feed(malformed, sizeof(malformed));
    assert(lis.errors == 2);
    assert(lis.payloads.empty());
    std::cout << "PASS" << std::endl;
}

void test_oversize_frame()
{
    std::cout << "\n=== Test: Oversize Frame Overflow ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    std::vector<uint8_t> flood(300, 0xCC);
    rx.feed(flood.data(), (int)flood.size());
    assert(lis.errors >= 1 && lis.payloads.empty());
    rx.reset();
    std::vector<uint8_t> p = { 0x42 };
    auto w = wireFrame(p, 3);
    rx.feed(w.data(), (int)w.size());
    assert(lis.payloads.size() == 1 && lis.payloads[0] == p);
    assert(lis.seqs[0] == 3);
    std::cout << "PASS" << std::endl;
}

void test_keepalive_atom_skipped()
{
    std::cout << "\n=== Test: 0x00 0x00 Keepalive Atom Skipped ==="
              << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    uint8_t atom[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    rx.feed(atom, sizeof(atom));
    assert(lis.payloads.empty() && lis.errors == 0);
    std::cout << "PASS" << std::endl;
}

void test_keepalive_atom_split_across_feeds()
{
    std::cout << "\n=== Test: 0x00 0x00 Split Across Feeds ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    uint8_t a = 0x00, b = 0x00;
    rx.feed(&a, 1);
    assert(lis.payloads.empty() && lis.errors == 0);
    rx.feed(&b, 1);
    assert(lis.payloads.empty() && lis.errors == 0);
    std::cout << "PASS" << std::endl;
}

void test_keepalive_after_partial_frame()
{
    std::cout << "\n=== Test: 0x00 0x00 After Partial Frame ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    uint8_t partial_then_atom[] = { 0x05, 0x11, 0x22, 0x00, 0x00 };
    rx.feed(partial_then_atom, sizeof(partial_then_atom));
    assert(lis.payloads.empty() && lis.errors == 1);
    std::cout << "PASS" << std::endl;
}

void test_frames_around_keepalive()
{
    std::cout << "\n=== Test: Frames Around Keepalive Atoms ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    std::vector<uint8_t> p = { 0xAB, 0xCD, 0xEF };
    auto w = wireFrame(p, 9);
    std::vector<uint8_t> ev = { 0x00, 0x00 };
    ev.insert(ev.end(), w.begin(), w.end());
    ev.insert(ev.end(), { 0x00, 0x00 });
    rx.feed(ev.data(), (int)ev.size());
    assert(lis.payloads.size() == 1);
    assert(lis.payloads[0] == p);
    assert(lis.seqs[0] == 9);
    assert(lis.errors == 0);
    std::cout << "PASS" << std::endl;
}

void test_lone_zero_still_skipped()
{
    std::cout << "\n=== Test: Lone Stray 0x00 Still Skipped ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    std::vector<uint8_t> p = { 0x42, 0x43 };
    auto w = wireFrame(p, 4);
    std::vector<uint8_t> ev = { 0x00 };
    ev.insert(ev.end(), w.begin(), w.end());
    ev.push_back(0x00);
    rx.feed(ev.data(), (int)ev.size());
    assert(lis.payloads.size() == 1 && lis.payloads[0] == p);
    assert(lis.seqs[0] == 4);
    assert(lis.errors == 0);
    std::cout << "PASS" << std::endl;
}

void test_drop_stops_feed_early()
{
    std::cout << "\n=== Test: Listener Drop Stops Feed Early ===" << std::endl;
    MockListener lis;
    lis.dropAfterErrors = 2;
    UtilFrameRx rx(lis);

    std::vector<uint8_t> ev = { 0x00, 0x02, 0xAB, 0x00, 0x02, 0xCD,
                                0x00, 0xAA, 0x55, 0x22, 0x99 };
    int used = rx.feed(ev.data(), (int)ev.size());
    assert(lis.errors == 2);
    assert(used == 7);
    std::cout << "PASS" << std::endl;
}

void test_reset_discards_partial()
{
    std::cout << "\n=== Test: reset() Discards Partial Frame ===" << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    uint8_t partial[] = { 0x05, 0x11, 0x22 };
    rx.feed(partial, sizeof(partial));
    rx.reset();
    uint8_t delim = 0x00;
    rx.feed(&delim, 1);
    assert(lis.payloads.empty() && lis.errors == 0);
    std::cout << "PASS" << std::endl;
}

void test_zero_byte_payload_with_cobsSeq()
{
    std::cout << "\n=== Test: Zero-Byte Payload Delivered with cobsSeq ==="
              << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    auto w = wireFrame({}, 5);
    rx.feed(w.data(), (int)w.size());
    assert(lis.payloads.size() == 1 && lis.payloads[0].empty());
    assert(lis.seqs[0] == 5);
    assert(lis.errors == 0);
    std::cout << "PASS" << std::endl;
}

void test_max_cobsSeq()
{
    std::cout << "\n=== Test: cobsSeq=0xFD (max data) Passes Through ==="
              << std::endl;
    MockListener lis;
    UtilFrameRx rx(lis);
    std::vector<uint8_t> p = { 0xAB };

    auto wMax = wireFrame(p, 0xFD);
    rx.feed(wMax.data(), (int)wMax.size());
    assert(lis.payloads.size() == 1 && lis.payloads[0] == p);
    assert(lis.seqs[0] == 0xFD);

    std::cout << "\n=== Test: cobsSeq=0xFE (NAK_TYPE) Reserved ==="
              << std::endl;
    MockListener lisNak;
    UtilFrameRx rxNak(lisNak);
    auto wNak = wireFrame({ 0xAB }, 0xFE);
    rxNak.feed(wNak.data(), (int)wNak.size());
    assert(lisNak.payloads.empty());
    assert(lisNak.naks.size() == 1 && lisNak.naks[0] == 0xAB);

    std::cout << "\n=== Test: cobsSeq=0xFF (ACK_TYPE) Reserved ==="
              << std::endl;
    MockListener lisAck;
    UtilFrameRx rxAck(lisAck);
    auto wAck = wireFrame({ 0xAB }, 0xFF);
    rxAck.feed(wAck.data(), (int)wAck.size());
    assert(lisAck.payloads.empty());
    assert(lisAck.acks.size() == 1 && lisAck.acks[0] == 0xAB);
    std::cout << "PASS" << std::endl;
}

int main()
{
    std::cout << "=== Running UtilFrameRx Tests ===" << std::endl;
    test_single_frame();
    test_split_across_feeds();
    test_back_to_back_frames();
    test_bad_crc_is_error();
    test_malformed_and_crc_only_are_errors();
    test_oversize_frame();
    test_keepalive_atom_skipped();
    test_keepalive_atom_split_across_feeds();
    test_keepalive_after_partial_frame();
    test_frames_around_keepalive();
    test_lone_zero_still_skipped();
    test_drop_stops_feed_early();
    test_reset_discards_partial();
    test_zero_byte_payload_with_cobsSeq();
    test_max_cobsSeq();
    std::cout << "\n=== UtilFrameRx Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif