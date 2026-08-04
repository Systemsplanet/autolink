// Message-codec pins: header encode/decode roundtrip, beginMsg
// bounds, resync-scan truth table. Toggle any codec rule -> red.
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>
#include "al/link/io/LinkMsgCodec.h"
#include "al/util/UtilCrc.h"

using namespace autolink;

static void test_hdr_roundtrip() {
    std::cout << "\n=== Pin 1: header encode/decode roundtrip ===" << std::endl;
    const uint32_t lens[] = { 1, 2, 255, 256, 65535, 70000, 0x7FFFFFFF };
    const uint16_t crcs[] = { 0, 1, 0xABCD, 0xFFFF };
    for (uint32_t L : lens)
        for (uint16_t c : crcs) {
            uint8_t h[MSG_HDR];
            msgHdrEncode(L, c, h);
            MsgHdr m = msgHdrDecode(h);
            assert(m.len == L && m.crc == c);
        }
    std::cout << "  PASS" << std::endl;
}

static void test_beginMsg_bounds() {
    std::cout << "\n=== Pin 2: beginMsg bounds ===" << std::endl;
    uint8_t h[MSG_HDR];
    LinkMsgCodec cx;
    msgHdrEncode(0, 0x1111, h);
    assert(!cx.beginMsg(h, 2048) && "len 0 must be rejected");
    msgHdrEncode(2049, 0x1111, h);
    assert(!cx.beginMsg(h, 2048) && "len > maxMsg must be rejected");
    assert(!cx.inMsg());
    msgHdrEncode(2048, 0x2222, h);
    assert(cx.beginMsg(h, 2048) && "len == maxMsg is in bounds");
    assert(cx.inMsg() && cx.len() == 2048 && cx.crc() == 0x2222);
    cx.reset();
    assert(!cx.inMsg());
    std::cout << "  PASS" << std::endl;
}

static void appendMsg(std::vector<uint8_t> &v, const uint8_t *b, int n) {
    uint8_t h[MSG_HDR];
    msgHdrEncode((uint32_t)n, UtilCrc::crc16(b, n), h);
    v.insert(v.end(), h, h + MSG_HDR);
    v.insert(v.end(), b, b + n);
}

static void test_resync_scan() {
    std::cout << "\n=== Pin 3: resync-scan truth table ===" << std::endl;
    uint8_t body[32];
    for (int i = 0; i < 32; i++)
        body[i] = (uint8_t)(i * 7);

    std::vector<uint8_t> v;
    appendMsg(v, body, 32);
    assert(msgResyncScan(v.data(), (int)v.size(), 2048) == 0 &&
           "clean header at 0");

    std::vector<uint8_t> j = { 0x13, 0x37, 0xAA };
    j.insert(j.end(), v.begin(), v.end());
    assert(msgResyncScan(j.data(), (int)j.size(), 2048) == 3 &&
           "header found past junk");

    std::vector<uint8_t> junkOnly(64, 0xEE);
    assert(msgResyncScan(junkOnly.data(), 64, 2048) == -1 && "no header");

    // Body truncated: header present but crc can't verify.
    std::vector<uint8_t> t(v.begin(), v.end() - 4);
    assert(msgResyncScan(t.data(), (int)t.size(), 2048) == -1 &&
           "truncated body must not match");

    // Corrupted body: crc mismatch skips this candidate.
    std::vector<uint8_t> c = v;
    c[MSG_HDR + 5] ^= 0x40;
    assert(msgResyncScan(c.data(), (int)c.size(), 2048) == -1 &&
           "crc mismatch must not match");

    assert(msgResyncScan(v.data(), MSG_HDR - 1, 2048) == -1 &&
           "shorter than a header");
    std::cout << "  PASS" << std::endl;
}

int main() {
    test_hdr_roundtrip();
    test_beginMsg_bounds();
    test_resync_scan();
    std::cout << "\nLinkMsgCodecTest: all pins passed" << std::endl;
    return 0;
}
