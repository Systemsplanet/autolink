// ARQ cache in isolation. Split out of
// the AutoLink facade. The trampolines
// are gone; the cache is a standalone
// class the link borrows via IArqCache.
// The contract pins from the old
// ArqCacheHasRoomTrampoline
// test (pool-bound, slot-bound, fresh
// is empty) carry over directly, plus
// the new mutator/peek/clear surface.
#ifndef ARDUINO

#    include <cassert>
#    include <cstring>
#    include <iostream>
#    include "al/link/arq/ArqCache.h"
#    include "NullArqCache.h"

using namespace autolink;

namespace {
uint8_t makePayload(int seed, int n) {
    return (uint8_t)((seed * 31 + n * 7) & 0xFF);
}
} // namespace

void test_fresh_cache_has_room() {
    std::cout << "\n=== Test: fresh ArqCache reports hasRoom() ==="
              << std::endl;
    ArqCache c;
    assert(c.hasRoom());
    assert(c.size() == 0);
    std::cout << "PASS (empty cache -> hasRoom + size 0)" << std::endl;
}

void test_pool_exhausted_means_no_room() {
    std::cout << "\n=== Test: hasRoom() is pool-bound, not slot-bound ==="
              << std::endl;
    ArqCache c;
    c.testFillPool();
    // pendingCount_ is still 0, but the
    // pool is full — hasRoom() must
    // return false. The old trampoline
    // bug surfaced here: a cache that
    // only checked pendingCount_ would
    // say "yes" and let sendMsg write
    // past the pool. Pin it.
    assert(!c.hasRoom());
    std::cout << "PASS (pool=" << ArqCache::POOL_SIZE
              << " full, slot count 0 -> not hasRoom)" << std::endl;
}

void test_slots_full_means_no_room() {
    std::cout << "\n=== Test: SLOTS full -> not hasRoom even with pool free ==="
              << std::endl;
    ArqCache c;
    c.testFillSlots();
    assert(!c.hasRoom());
    std::cout << "PASS (slots=" << ArqCache::SLOTS << " full -> not hasRoom)"
              << std::endl;
}

void test_insert_then_free() {
    std::cout << "\n=== Test: insert + freeBySeq roundtrip ===" << std::endl;
    ArqCache c;
    uint8_t buf[32];
    for (int i = 0; i < 32; i++)
        buf[i] = makePayload(1, i);
    c.insert(7, buf, 32);
    assert(c.size() == 1);
    assert(c.slotInUse(7));
    c.freeBySeq(7);
    assert(c.size() == 0);
    assert(!c.slotInUse(7));
    std::cout << "PASS" << std::endl;
}

void test_insert_replace_reuses_pool() {
    std::cout
        << "\n=== Test: insert over an existing slot returns the pool buf ==="
        << std::endl;
    ArqCache c;
    uint8_t a[16];
    uint8_t b[16];
    for (int i = 0; i < 16; i++) {
        a[i] = makePayload(1, i);
        b[i] = makePayload(2, i);
    }
    c.insert(3, a, 16);
    int sizeAfterFirst = c.size();
    c.insert(3, b, 16);
    int sizeAfterReplace = c.size();
    assert(sizeAfterFirst == 1);
    assert(sizeAfterReplace == 1);
    // After replace, peekForRetx must
    // return the second payload's bytes,
    // not the first.
    const uint8_t *out = nullptr;
    int len = 0;
    assert(c.peekForRetx(3, &out, &len));
    assert(len == 16);
    assert(memcmp(out, b, 16) == 0);
    c.freeBySeq(3);
    std::cout << "PASS" << std::endl;
}

void test_peek_miss_returns_false() {
    std::cout << "\n=== Test: peekForRetx misses on absent seq ==="
              << std::endl;
    ArqCache c;
    const uint8_t *out = nullptr;
    int len = 0;
    assert(!c.peekForRetx(200, &out, &len));
    assert(out == nullptr);
    assert(len == 0);
    std::cout << "PASS" << std::endl;
}

void test_peek_keepalive_is_no_buf() {
    std::cout << "\n=== Test: peekForRetx on a keepalive slot returns false ==="
              << std::endl;
    ArqCache c;
    // payload == nullptr simulates a
    // zero-length (keepalive) insert.
    c.insert(5, nullptr, 0);
    assert(c.slotInUse(5));
    const uint8_t *out = nullptr;
    int len = 0;
    assert(!c.peekForRetx(5, &out, &len));
    std::cout << "PASS (slot in_use but no pool buf -> retx no-op)"
              << std::endl;
}

void test_peek_does_not_borrow_across_free() {
    std::cout << "\n=== Test: peekForRetx buffer invalidated by freeBySeq ==="
              << std::endl;
    ArqCache c;
    uint8_t buf[16];
    for (int i = 0; i < 16; i++)
        buf[i] = makePayload(7, i);
    c.insert(11, buf, 16);
    const uint8_t *out = nullptr;
    int len = 0;
    assert(c.peekForRetx(11, &out, &len));
    const uint8_t *outCopy = out;
    c.freeBySeq(11);
    // After freeBySeq, the slot is
    // empty. peekForRetx must now miss.
    assert(!c.peekForRetx(11, &out, &len));
    // The caller must not retain outCopy
    // across the free — but the API
    // can't enforce that. The test
    // documents the contract.
    (void)outCopy;
    std::cout << "PASS (peek miss after free; borrowed ptr not retained)"
              << std::endl;
}

void test_clearAll_resets_state() {
    std::cout << "\n=== Test: clearAll drops every slot and pool buffer ==="
              << std::endl;
    ArqCache c;
    uint8_t buf[16];
    for (int i = 0; i < 16; i++)
        buf[i] = makePayload(9, i);
    for (int i = 0; i < 20; i++)
        c.insert((uint8_t)(i * 13), buf, 16);
    assert(c.size() == 20);
    c.clearAll();
    assert(c.size() == 0);
    for (int i = 0; i < 256; i++)
        assert(!c.slotInUse((uint8_t)i));
    // After clearAll, hasRoom() must
    // return true again — pool + slots
    // both free.
    assert(c.hasRoom());
    std::cout << "PASS" << std::endl;
}

void test_pool_exhaustion_skips_insert() {
    std::cout
        << "\n=== Test: insert with full pool logs and skips (retx = miss) ==="
        << std::endl;
    ArqCache c;
    // Fill every pool buffer with a
    // real payload. Slot count must
    // match the pool size.
    uint8_t buf[8];
    for (int i = 0; i < 8; i++)
        buf[i] = makePayload(2, i);
    for (int i = 0; i < ArqCache::POOL_SIZE; i++)
        c.insert((uint8_t)i, buf, 8);
    assert(c.size() == ArqCache::POOL_SIZE);
    assert(!c.hasRoom());
    // One more insert must be a no-op:
    // size stays at POOL_SIZE, the
    // pool buffer is left untouched.
    c.insert(0, buf, 8);
    assert(c.size() == ArqCache::POOL_SIZE);
    std::cout << "PASS" << std::endl;
}

void test_oversize_payload_skips() {
    std::cout << "\n=== Test: payload > POOL_BUF_MAX is rejected ==="
              << std::endl;
    ArqCache c;
    uint8_t big[ArqCache::POOL_BUF_MAX + 16];
    memset(big, 0xAB, sizeof big);
    c.insert(3, big, (int)sizeof big);
    // Slot is not in_use; the
    // oversized payload was rejected.
    assert(!c.slotInUse(3));
    assert(c.size() == 0);
    std::cout << "PASS" << std::endl;
}

void test_hasRoom_is_O1_after_pool_drain() {
    std::cout
        << "\n=== Test: hasRoom() stays O(1) as the pool fills (no scan) ==="
        << std::endl;
    // Regression: hasRoom() used to
    // scan all POOL_SIZE entries. At
    // WINDOW=32 it was tolerable but
    // a real bottleneck the moment
    // POOL_SIZE grew. The fix
    // (poolFree_ counter) makes hasRoom
    // O(1) at any pool occupancy.
    //
    // We can't time inside a subsecond
    // host suite, but we *can* prove
    // hasRoom() tracks the right
    // answer across every intermediate
    // state of fill/drain. If the
    // counter drifts from poolUsed_,
    // the answer flips wrong here and
    // assertInvariants() (gated on
    // AUTOLINK_HOST_TEST, on in this
    // build) fires.
    ArqCache c;
    uint8_t payload[16] = {};
    // Drain the pool one buffer at a
    // time and check hasRoom after
    // each transition. Fresh -> true.
    assert(c.hasRoom());
    for (int i = 0; i < ArqCache::POOL_SIZE; i++) {
        c.insert((uint8_t)i, payload, 16);
        if (i + 1 < ArqCache::POOL_SIZE)
            assert(c.hasRoom());
    }
    assert(!c.hasRoom());
    // Free one slot mid-pool: hasRoom
    // must flip back to true without a
    // scan. Free the seq with the
    // smallest index that is in_use.
    c.freeBySeq(0);
    assert(c.hasRoom());
    // Free the rest. Every step the
    // answer must be right.
    for (int i = 1; i < ArqCache::POOL_SIZE; i++)
        c.freeBySeq((uint8_t)i);
    assert(c.hasRoom());
    assert(c.size() == 0);
    std::cout << "PASS (hasRoom tracks state across full fill + drain)"
              << std::endl;
}

void test_poolFree_track_matches_scan_after_replace() {
    std::cout
        << "\n=== Test: poolFree_ stays in sync through insert-over-existing "
           "==="
        << std::endl;
    // Replacing an existing slot must
    // release the old pool buffer back
    // to the free pool before claiming
    // a new one. Otherwise poolFree_
    // would drift negative (or, with
    // the clamp, stay at 0) and a
    // subsequent hasRoom() would lie.
    ArqCache c;
    uint8_t payload[16] = {};
    c.insert(7, payload, 16);
    assert(c.hasRoom());
    // Replace 30 times — every replace
    // must hand the pool buffer back.
    for (int i = 0; i < 30; i++) {
        c.insert(7, payload, 16);
        assert(c.hasRoom());
    }
    // And free / re-insert must keep
    // the counter honest.
    c.freeBySeq(7);
    assert(c.hasRoom());
    // Pool is fully empty — fill it
    // and check hasRoom flips at the
    // boundary.
    for (int i = 0; i < ArqCache::POOL_SIZE; i++)
        c.insert((uint8_t)i, payload, 16);
    assert(!c.hasRoom());
    std::cout << "PASS (replace does not leak pool buffers into poolFree_)"
              << std::endl;
}

void test_hasRoom_after_clearAll_resets_to_true() {
    std::cout << "\n=== Test: clearAll restores poolFree_ to POOL_SIZE ==="
              << std::endl;
    // clearAll zeros the pool. The
    // counter must follow — otherwise
    // hasRoom() would say false on a
    // freshly reset link and stall the
    // first post-reset send.
    ArqCache c;
    uint8_t payload[16] = {};
    for (int i = 0; i < ArqCache::POOL_SIZE; i++)
        c.insert((uint8_t)i, payload, 16);
    assert(!c.hasRoom());
    c.clearAll();
    assert(c.hasRoom());
    assert(c.size() == 0);
    std::cout << "PASS (poolFree_ reset to POOL_SIZE on clearAll)" << std::endl;
}

void test_poolFree_zero_means_insert_skips() {
    std::cout
        << "\n=== Test: poolFree_=0 causes insert to log + skip (no drift) ==="
        << std::endl;
    // Direct check on the O(1) fast
    // path: with poolFree_ at 0 (set by
    // testFillPool, not by scan), the
    // next real insert must hit the
    // "pool exhausted" branch and leave
    // state untouched. Reverting to
    // the linear-scan implementation
    // (where the poolUsed_ bitmap
    // alone gates insert) would still
    // pass — the regression pin is the
    // assertInvariants() cross-check
    // that fires inside insert(), which
    // only fires when the counter and
    // the bitmap disagree. Toggle off
    // the poolFree_ bookkeeping ->
    // invariant on every insert
    // explodes. Toggle on -> green.
    ArqCache c;
    c.testFillPool();
    assert(!c.hasRoom());
    uint8_t payload[16] = { 0xAB };
    int sizeBefore = c.size();
    c.insert(0, payload, 16);
    assert(c.size() == sizeBefore);
    assert(!c.slotInUse(0));
    std::cout << "PASS" << std::endl;
}

void test_pool_size_covers_pipeline_window() {
    std::cout
        << "\n=== Test: pool >= 2*WINDOW (retx headroom over pipeline) ==="
        << std::endl;
    // The link borrows the cache by
    // pointer; the ping pipeline fires
    // WINDOW chunks into it before any
    // ACK frees a slot, and unacked
    // slots stay held while retx
    // copies them out. Pool must hold a
    // full window plus retx headroom.
    // Toggling WINDOW up or POOL_SIZE
    // down fails the static_assert in
    // ArqCache.h at compile time; this
    // test pins the runtime behaviour.
    if (ArqCache::POOL_SIZE < ArqCache::WINDOW * 2) {
        std::cerr << "\nFAIL: ArqCache::POOL_SIZE=" << ArqCache::POOL_SIZE
                  << " must be >= 2*WINDOW=" << (ArqCache::WINDOW * 2)
                  << ". With zero margin, the link's retx of unacked slots "
                     "exhausts the pool and insert() silently drops the "
                     "next send."
                  << std::endl;
        assert(false);
    }

    // Behavioural pin: the cache can
    // absorb a full pipeline window plus
    // WINDOW retx-held neighbours before
    // any insert silently drops. The
    // (WINDOW*2 + 1)-th insert is the
    // boundary that should fail.
    ArqCache c;
    uint8_t payload[16] = {};
    for (int i = 0; i < ArqCache::WINDOW * 2; i++)
        c.insert((uint8_t)i, payload, 16);
    assert(c.size() == ArqCache::WINDOW * 2);
    assert(!c.hasRoom());
    std::cout << "PASS (POOL_SIZE=" << ArqCache::POOL_SIZE
              << " >= 2*WINDOW=" << (ArqCache::WINDOW * 2) << ")" << std::endl;
}

int main() {
    std::cout << "=== Running ArqCache Tests ===" << std::endl;
    test_fresh_cache_has_room();
    test_pool_exhausted_means_no_room();
    test_slots_full_means_no_room();
    test_insert_then_free();
    test_insert_replace_reuses_pool();
    test_peek_miss_returns_false();
    test_peek_keepalive_is_no_buf();
    test_peek_does_not_borrow_across_free();
    test_clearAll_resets_state();
    test_pool_exhaustion_skips_insert();
    test_oversize_payload_skips();
    test_hasRoom_is_O1_after_pool_drain();
    test_poolFree_track_matches_scan_after_replace();
    test_hasRoom_after_clearAll_resets_to_true();
    test_poolFree_zero_means_insert_skips();
    test_pool_size_covers_pipeline_window();
    std::cout << "\n=== ArqCache Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif
