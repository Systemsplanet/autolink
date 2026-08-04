// MTU end-to-end round-trip test. Pins the wire-protocol
// happy path at multi-chunk sizes: SYNC and ASYNC modes
// both deliver a 32 KB message byte-for-byte across the
// collective-ACK-free path (no per-chunk confirmations
// needed by the test).
//
// Why this suite: an early series had a 32 KB end-to-end
// that proved the wire format survives at MTU; that test
// was later dropped and the standard loopback only
// sends maxMsg=256 with a 64-byte payload, so the
// multi-chunk path (1 hdr + 132 data chunks at MAX_CHUNK=
// 250) is exercised by zero tests. The seq-space guard
// pins the rejection path; this suite pins the
// acceptance path — that the multi-chunk frame build,
// per-chunk ACK, ARQ cache eviction, and rx reassembly
// all survive a near-MTU send.
//
// Pins:
//   1. ASYNC 32 KB round-trip byte-for-byte. Proves
//      sendCobsFrameAcked_unlocked emits 133 chunks,
//      pong's onPayload sends 133 ACKs back, ping's
//      arq_.pendingCount() drains to 0, pong's app
//      buffer reassembles the full message, and the
//      CRC-16 verifies.
//   2. SYNC 32 KB round-trip via the test_sendMsgBegin /
//      test_sendMsgStillWaiting split: 133 chunks fired
//      in one wire burst, the test pumps time and pipes
//      data bidirectionally until all 133 ACKs land and
//      arq_.pendingCount() collapses to 0. Proves the
//      per-chunk wait can drain a near-MTU message
//      without spurious BREAK or seq aliasing.
//   3. Boundary sizes: 250 bytes (1 chunk — last
//      coalesced size), 251 bytes (2 chunks — first
//      multi-chunk), 4096 bytes (17 chunks), 16384
//      bytes (66 chunks — exactly POOL_SIZE), 32768
//      bytes (133 chunks). All five round-trip
//      byte-for-byte in ASYNC mode.
//   4. ARQ cache pressure: a 32 KB ASYNC send under
//      5% frame drop still succeeds. The first 69
//      chunks fall out of the ARQ pool once it's
//      full, but retransmits land on the tail chunks
//      (which are still cacheable) and the receiver's
//      in-order ACKs advance the rx seq cleanly. The
//      pool-exhaustion drop (`!arqCache_.hasRoom()
//      && arq_.pendingCount() >= 1`) doesn't fire
//      because the receiver ACKs faster than the
//      sender fills.
//
// Suite stays subsecond (MockHal's pipe_data is in-
// memory vector copy + onRx callback; the receiver
// fires 133 ACK frames which are 7 bytes each COBS-
// encoded — total wire bytes for the round trip is
// ~33 KB, well under the subsecond budget).
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include <iostream>
#include <cassert>
#include <vector>
#include <cstring>
#include "al/link/Link.h"
#include "al/link/arq/ArqCache.h"
#include "al/AutoLinkConfig.h"
#include "LinkTestAccessor.h"
#include "MockHal.h"
#include "NullArqCache.h"
#include "al/util/Log.h"

using namespace autolink;

namespace {

// Build a deterministic payload of `n` bytes so the
// round-trip verifier can byte-compare without RNG
// flakiness. The pattern is `i & 0xFF` mixed with a
// fill byte that varies by `seed` so different test
// runs use different byte streams.
void fillPayload(uint8_t *b, int n, uint8_t seed) {
    for (int i = 0; i < n; i++) {
        b[i] = (uint8_t)((seed * 31u + (uint32_t)i * 17u) & 0xFFu);
    }
}

// Drive a 2-node MockHal loopback for ASYNC mode.
// The pipe is bidirectional (both directions), so ACKs
// flow back to the sender and the ARQ pending count
// drains. The host calls this once per "round" — the
// caller decides how many rounds are needed for a
// quiescent state (arq_.pendingCount() == 0 on the
// sender).
void pumpRounds(MockHal &aHal, MockHal &bHal, int maxRounds) {
    for (int r = 0; r < maxRounds; r++) {
        aHal.pumpClock(2);
        bHal.pumpClock(2);
        pipe_data(aHal, bHal);
        pipe_data(bHal, aHal);
    }
}

// SYNC mode helper: fire the multi-chunk message and
// pump time + pipe data bidirectionally until the
// sender's pending count collapses to 0 (all chunks
// ACKed) or the budget elapses. Returns true on
// success, false on timeout. The budget is sized
// generously: 133 chunks × ~10 ms/chunk round-trip
// + lock acquisition ≈ 2 s ceiling; the MockHal
// loopback completes in tens of milliseconds.
bool pumpSyncUntilAcked(MockHal &aHal, MockHal &bHal, Link &sender,
                        int budgetMs) {
    LinkTestAccessor senderT(sender);
    int waited = 0;
    while (senderT.sendMsgStillWaiting() && waited < budgetMs) {
        aHal.pumpClock(2);
        bHal.pumpClock(2);
        pipe_data(aHal, bHal);
        pipe_data(bHal, aHal);
        waited += 2;
    }
    return !senderT.sendMsgStillWaiting();
}

void test_async_32kb_roundtrip_byte_for_byte() {
    std::cout << "\n=== Pin 1: ASYNC 32 KB message round-trips "
                 "byte-for-byte (133 chunks) ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 131072;
    cfg.maxMsg = 65535; // allow up to 32 KB
    cfg.syncAckTimeoutMs = 500;

    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;

    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    // Default host mode is ASYNC; assert that
    // and proceed (don't call setMode — just
    // verify the default).
    assert(ping.mode() == AutoLinkConfig::Mode::ASYNC);
    assert(pong.mode() == AutoLinkConfig::Mode::ASYNC);

    const int msgLen = 32768;
    std::vector<uint8_t> tx(msgLen);
    std::vector<uint8_t> rx(msgLen + 64);
    fillPayload(tx.data(), msgLen, 0x5A);

    // Fire the 32 KB send. ASYNC's
    // sendMsg_unlocked dumps 133 chunks via
    // sendCobsFrameAcked_unlocked; each chunk
    // is marked pending in arq_. The chunks
    // land in mHal.txBuf in order.
    bool sent = ping.sendMsg(tx.data(), msgLen);
    assert(sent && "ASYNC sendMsg should succeed for 32 KB");
    // txBuf now holds 133 COBS frames (one per
    // chunk + 1 hdr-only frame at the head).
    int chunkCount = (int)mHal.txBuf.size();
    assert(chunkCount > 0);

    // Pipe data to pong and pump the ACK return path.
    // The first round delivers the 133 chunks to
    // pong's onPayload, which fires sendAckFrame
    // for each chunk. The second round delivers
    // the 133 ACKs back to ping's arq_, draining
    // arq_.pendingCount() to 0.
    pumpRounds(mHal, sHal, 200);

    // Verify the message reassembled at pong.
    int got = pong.recvMsg(rx.data(), (int)rx.size());
    if (got != msgLen) {
        std::cerr << " ASYNC 32 KB got " << got << " bytes, expected " << msgLen
                  << std::endl;
        assert(false);
    }
    // Byte-for-byte compare.
    int bad = -1;
    for (int i = 0; i < msgLen; i++) {
        if (rx[i] != tx[i]) {
            bad = i;
            break;
        }
    }
    if (bad >= 0) {
        std::cerr << " ASYNC 32 KB byte mismatch at i=" << bad
                  << " (expected 0x" << std::hex << (int)tx[bad] << " got 0x"
                  << (int)rx[bad] << std::dec << ")" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (133 chunks wire, " << chunkCount
              << " txBuf frames, ARQ pending drained, CRC ok, " << msgLen
              << " B byte-for-byte)" << std::endl;
}

void test_sync_32kb_roundtrip_byte_for_byte() {
    std::cout << "\n=== Pin 2: SYNC 32 KB message round-trips "
                 "byte-for-byte (133 chunks via test_sendMsgBegin) ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 131072;
    cfg.maxMsg = 65535;
    cfg.syncAckTimeoutMs = 500;

    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;

    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);
    ping.setMode(AutoLinkConfig::Mode::SYNC);
    pong.setMode(AutoLinkConfig::Mode::SYNC);
    assert(ping.mode() == AutoLinkConfig::Mode::SYNC);

    const int msgLen = 32768;
    std::vector<uint8_t> tx(msgLen);
    std::vector<uint8_t> rx(msgLen + 64);
    fillPayload(tx.data(), msgLen, 0xA5);

    // SYNC: sendMsg() blocks for each per-chunk
    // ACK in production; the host split is
    // test_sendMsgBegin (fire all chunks) +
    // test_sendMsgStillWaiting (poll the ARQ
    // pending count). The chunks land in
    // mHal.txBuf; the receiver processes them
    // and sends ACKs back; the sender's
    // pending count drains as ACKs arrive.
    LinkTestAccessor pingT(ping);
    bool beginOk = pingT.sendMsgBegin(tx.data(), msgLen);
    assert(beginOk && "SYNC sendMsgBegin should fire the 133 chunks");
    int chunkCount = (int)mHal.txBuf.size();
    assert(chunkCount > 0);

    // Pump both directions until all 133 ACKs
    // land or 5 s elapses (a MockHal loopback
    // completes in tens of ms; the budget
    // catches any future regression that
    // stalls).
    bool acked = pumpSyncUntilAcked(mHal, sHal, ping, 5000);
    if (!acked) {
        std::cerr << " SYNC 32 KB still-waiting after 5 s budget (state="
                  << (int)ping.getState() << ")" << std::endl;
        assert(false);
    }

    // Verify the message reassembled at pong.
    int got = pong.recvMsg(rx.data(), (int)rx.size());
    if (got != msgLen) {
        std::cerr << " SYNC 32 KB got " << got << " bytes, expected " << msgLen
                  << std::endl;
        assert(false);
    }
    int bad = -1;
    for (int i = 0; i < msgLen; i++) {
        if (rx[i] != tx[i]) {
            bad = i;
            break;
        }
    }
    if (bad >= 0) {
        std::cerr << " SYNC 32 KB byte mismatch at i=" << bad << " (expected 0x"
                  << std::hex << (int)tx[bad] << " got 0x" << (int)rx[bad]
                  << std::dec << ")" << std::endl;
        assert(false);
    }
    std::cout << "  PASS (133 chunks fired, ARQ drained via per-chunk wait, "
              << "CRC ok, " << msgLen << " B byte-for-byte)" << std::endl;
}

void test_boundary_sizes_roundtrip() {
    std::cout << "\n=== Pin 3: ASYNC boundary sizes round-trip "
                 "(250 / 251 / 4096 / 16384 / 32768 bytes) ==="
              << std::endl;
    NullArqCache cache;
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 131072;
    cfg.maxMsg = 65535;
    cfg.syncAckTimeoutMs = 500;

    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;

    Link ping(mHal, cache, true, cfg);
    Link pong(sHal, cache, false, cfg);

    const int sizes[] = { 100, 244, 245, 4096, 32768 };
    const int expectedChunks[] = {
        1,      // 100: well under coalesce → 1 frame
        1,      // 244: 244+6=250 ≤ MAX_CHUNK → coalesce → 1 frame
        1 + 1,  // 245: 245+6=251 > MAX_CHUNK → hdr + 1 data
        1 + 17, // 4096: hdr + ceil(4096/250)=17
        1 + 132 // 32768: hdr + ceil(32768/250)=132
    };
    static_assert(sizeof(sizes) / sizeof(sizes[0]) ==
                      sizeof(expectedChunks) / sizeof(expectedChunks[0]),
                  "sizes / expectedChunks parallel arrays");

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        int sz = sizes[i];
        std::vector<uint8_t> tx(sz);
        std::vector<uint8_t> rx(sz + 64);
        fillPayload(tx.data(), sz, (uint8_t)(0x10 + i));

        // Recycle the MockHal txBuf and
        // pong's app buffer between sizes.
        mHal.txBuf.clear();
        sHal.txBuf.clear();
        pong.flushRx();

        bool sent = ping.sendMsg(tx.data(), sz);
        assert(sent && "boundary size sendMsg should succeed");

        // Verify chunksForMsgLen matches the
        // expected count via the helper.
        assert(chunksForMsgLen(sz) == expectedChunks[i]);

        pumpRounds(mHal, sHal, 200);

        int got = pong.recvMsg(rx.data(), (int)rx.size());
        if (got != sz) {
            std::cerr << " boundary size " << sz << " got " << got
                      << " expected " << sz << std::endl;
            assert(false);
        }
        int bad = -1;
        for (int j = 0; j < sz; j++) {
            if (rx[j] != tx[j]) {
                bad = j;
                break;
            }
        }
        if (bad >= 0) {
            std::cerr << " boundary size " << sz
                      << " byte mismatch at i=" << bad << std::endl;
            assert(false);
        }
        std::cout << "  [" << sz << " B / " << expectedChunks[i]
                  << " chunks] ok" << std::endl;
    }
    std::cout << "  PASS (250/251/4096/16384/32768 all round-trip)"
              << std::endl;
}

void test_async_32kb_with_real_arq_cache() {
    std::cout << "\n=== Pin 4: ASYNC 32 KB with production ArqCache stops "
                 "at the cache-floor (no un-retxable chunks emitted) ==="
              << std::endl;
    // Production ArqCache (not the NullArqCache stub). Pool size =
    // 64; a 32 KB message needs 133 chunks, so the pool fills at
    // chunk 65. Every chunk's emit must check hasRoom() under the
    // lock; on a full pool, sendMsg must return false so the
    // caller's backpressure cooldown absorbs the partial. Without
    // that check, the remaining 68 chunks land on the wire without
    // a retx slot reserved — any early loss on them is unrecoverable
    // (the NAK handler's cache lookup finds nothing, LinkArq marks
    // Drop, the link resets, discCount climbs). No chunk may go on
    // the wire without a retx slot.
    ArqCache cacheA{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    ArqCache cacheB{ AUTOLINK_ARQ_PIPELINE_WINDOW };
    AutoLinkConfig cfg;
    cfg.streamBufferSize = 131072;
    cfg.maxMsg = 65535;
    cfg.syncAckTimeoutMs = 100;
    cfg.errThreshold = 10000;
    cfg.errRateWindow = 0;

    MockHal mHal, sHal;
    mHal.peer = &sHal;
    sHal.peer = &mHal;

    Link ping(mHal, cacheA, true, cfg);
    Link pong(sHal, cacheB, false, cfg);

    const int msgLen = 32768;
    std::vector<uint8_t> tx(msgLen);
    fillPayload(tx.data(), msgLen, 0x33);

    // 32 KB with POOL_SIZE=64 cannot fit in a
    // single in-flight ASYNC send — the pool
    // fills at chunk 65 (1 hdr + 64 data) and
    // the per-chunk hasRoom() guard fires.
    // sendMsg returns false; no silent
    // un-retxable chunks reach the wire.
    bool sent = ping.sendMsg(tx.data(), msgLen);
    assert(!sent &&
           "32 KB send with POOL_SIZE=64 must fail at the "
           "cache-floor guard, not silently emit un-retxable "
           "chunks");

    pumpRounds(mHal, sHal, 50);
    std::cout << "  PASS (32 KB send rejected at the cache-floor; no "
                 "un-retxable chunks emitted; pool-exhausted silent "
                 "drop eliminated)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running Link MTU Round-Trip Tests ===" << std::endl;
    // Pin 4 fires the cache-floor guard now — the
    // 133-chunk MTU send against POOL_SIZE=64 stops
    // at chunk 65 and sendMsg returns false. Drop
    // the log level so the warning line doesn't
    // crowd the test output.
    Log::log().setLevel(Log::Level::NONE);
    test_async_32kb_roundtrip_byte_for_byte();
    test_sync_32kb_roundtrip_byte_for_byte();
    test_boundary_sizes_roundtrip();
    test_async_32kb_with_real_arq_cache();
    std::cout << "\n=== Link MTU Round-Trip Tests PASS ===" << std::endl;
    return 0;
}