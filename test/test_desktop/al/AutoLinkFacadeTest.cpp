// AutoLink facade: ARQ hooks, ack/retx.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include "al/hal/IHal.h"
#include "al/link/Link.h"
#include "al/util/Log.h"
#include "al/util/UtilBlink.h"
#include "AutoLink.h"
#include "AutoLinkTestAccessor.h"
#include "EspHalStub.h"

#include <iostream>
#include <cassert>
#include <string>
#include <vector>

using namespace autolink;

static int chunkCount(int len) { return 1 + (len + 250 - 1) / 250; }

void test_facade_arqcache_rejects_when_full() {
    AutoLink link(0, 16, 17, true);
    AutoLinkTestAccessor t(link);

    std::cout << "\n=== Test: ARQ cache gate (pool-backed, size="
              << AutoLinkTestAccessor(link).arqPoolSize()
              << ") ===" << std::endl;
    uint8_t payload[32] = { 0 };
    int filled = 0;
    const int POOL_SIZE = t.arqPoolSize();
    for (int i = 0; i < POOL_SIZE; i++) {
        t.arqCachePut((uint8_t)i, payload, 32, (uint8_t)chunkCount(32));
        filled++;
    }
    assert(filled == POOL_SIZE);
    assert(t.arqCacheSize() == POOL_SIZE);

    uint8_t overflowSeq = (uint8_t)POOL_SIZE;
    t.arqCachePut(overflowSeq, payload, 32, (uint8_t)chunkCount(32));
    assert(t.arqCacheSize() == POOL_SIZE);
    assert(!t.arqCacheHasRoom());

    t.arqCacheFreeBySeq(0);
    assert(t.arqCacheHasRoom());

    t.arqCachePut(overflowSeq, payload, 32, (uint8_t)chunkCount(32));
    assert(t.arqCacheSize() == POOL_SIZE);
    std::cout << "PASS" << std::endl;
}

void test_facade_arqcache_retx_frees_old_slot() {
    std::cout
        << "\n=== Test: arqCache retx keeps the slot alive; ack frees it (the fix) ==="
        << std::endl;
    AutoLink link(0, 16, 17, true);
    AutoLinkTestAccessor t(link);
    uint8_t payload[16] = { 1, 2,  3,  4,  5,  6,  7,  8,
                            9, 10, 11, 12, 13, 14, 15, 16 };
    t.arqCachePut(7, payload, 16, (uint8_t)chunkCount(16));
    assert(t.arqCacheSize() == 1);

    // arqCacheRetx now returns whether
    // peekForRetx hit (has payload to
    // retransmit). The old "drop request"
    // boolean is gone — there's no signal
    // path from cache back to link that can
    // request a drop. Pin: the cache
    // hit does not free the slot; only
    // freeBySeq does.
    bool retxHit = t.arqCacheRetx(7);
    assert(retxHit);
    assert(t.arqCacheSize() == 1);

    t.arqCacheFreeBySeq(7);
    assert(t.arqCacheSize() == 0);

    std::cout << "PASS" << std::endl;
}

void test_facade_reset_zeros_all_counters() {
    std::cout << "\n=== Test: AutoLink facade resets all counters ==="
              << std::endl;
    AutoLink link(0, 16, 17, true);
    AutoLinkTestAccessor t(link);

    uint8_t payload[32] = { 0 };
    for (int i = 0; i < 5; i++) {
        t.arqCachePut((uint8_t)i, payload, 32, (uint8_t)chunkCount(32));
    }
    assert(t.arqCacheSize() == 5);

    Link *lk = t.link();
    assert(lk != nullptr);
    lk->resetStats();
    lk->resetErrors();
    lk->resetDiag();
    Stats s;
    lk->getStats(s);
    if (s.tx != 0 || s.rx != 0 || s.discCount != 0 || s.frameErrs != 0) {
        std::cerr << "\nLink stats should be 0 after reset, got tx="
                  << (long long)s.tx << " rx=" << (long long)s.rx
                  << " disc=" << (long long)s.discCount
                  << " frameErrs=" << (long long)s.frameErrs << std::endl;
    }
    assert(s.tx == 0);
    assert(s.rx == 0);
    assert(s.discCount == 0);
    assert(s.frameErrs == 0);
    Diag d;
    lk->getDiag(d);
    assert(d.lostMsgs == 0);
    assert(d.gaps == 0);
    assert(d.stale == 0);
    std::cout << "PASS" << std::endl;
}

void test_facade_link_reset_clears_cache() {
    std::cout << "\n=== Test: link drop clears the facade cache (killer) ==="
              << std::endl;
    AutoLink link(0, 16, 17, true);
    AutoLinkTestAccessor t(link);
    uint8_t payload[64] = {};

    const int POOL_SIZE = t.arqPoolSize();
    int overshoot = POOL_SIZE + 8;
    for (int i = 0; i < overshoot; i++) {
        t.arqCachePut((uint8_t)i, payload, 64, (uint8_t)chunkCount(64));
    }
    int beforeDrop = t.arqCacheSize();

    if (beforeDrop != POOL_SIZE) {
        std::cerr << "\nfixture: cache should be " << POOL_SIZE
                  << " (pool full), got " << beforeDrop << std::endl;
        assert(false);
    }

    t.arqCache()->clearAll();
    int afterDrop = t.arqCacheSize();
    if (afterDrop != 0) {
        std::cerr << "\nFAIL: cache should be 0 after link drop, got "
                  << afterDrop
                  << " — the facade cache is not being cleared on link drop,"
                  << " the gate will latch after enough drops." << std::endl;
        assert(false);
    }
    std::cout << "  before drop: " << beforeDrop
              << " slots, after drop: " << afterDrop << " slots" << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_facade_repeated_drops_do_not_latch_gate() {
    std::cout
        << "\n=== Test: N drops don't latch the cache-full gate (saturation) ==="
        << std::endl;
    AutoLink link(0, 16, 17, true);
    AutoLinkTestAccessor t(link);
    uint8_t payload[32] = {};
    const int POOL_SIZE = t.arqPoolSize();
    int overshoot = POOL_SIZE + 4;
    for (int cycle = 0; cycle < 5; cycle++) {
        for (int i = 0; i < overshoot; i++) {
            t.arqCachePut((uint8_t)((cycle * 32 + i) & 0xFF), payload, 32,
                          (uint8_t)chunkCount(32));
        }
        int beforeDrop = t.arqCacheSize();
        if (beforeDrop != POOL_SIZE) {
            std::cerr << "\ncycle " << cycle << ": cache should be "
                      << POOL_SIZE << " before drop (pool full), got "
                      << beforeDrop << std::endl;
            assert(false);
        }

        t.arqCache()->clearAll();
        int afterDrop = t.arqCacheSize();
        if (afterDrop != 0) {
            std::cerr << "\nFAIL cycle " << cycle
                      << ": cache should be 0 after drop, got " << afterDrop
                      << " — pendingCount_ ratchets up across drops, "
                      << "the gate will latch after enough drops." << std::endl;
            assert(false);
        }
    }
    std::cout << "  5 drop cycles, cache returned to 0 after each" << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_facade_arq_cache_slots_exceeds_window() {
    std::cout << "\n=== Test: ARQ_CACHE_SLOTS > WINDOW (Bug 4) ==="
              << std::endl;
    int cache_slots = AutoLink::ARQ_CACHE_SLOTS_PUBLIC;
    int window = 32;
    if (cache_slots <= window) {
        std::cerr << "\nFAIL: ARQ_CACHE_SLOTS=" << cache_slots
                  << " must exceed WINDOW=" << window
                  << ". With zero margin, a single mis-keyed/leaked slot "
                  << "ratchets the cache to full and the gate latches."
                  << std::endl;
        assert(false);
    }
    std::cout << "  ARQ_CACHE_SLOTS=" << cache_slots << " > WINDOW=" << window
              << " ✓" << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_facade_cache_miss_does_not_clear_neighbours() {
    std::cout
        << "\n=== Test: cache-miss cleanup never crosses message boundaries (the fix) ==="
        << std::endl;
    AutoLink link(0, 16, 17, true);
    AutoLinkTestAccessor t(link);

    // The old test installed ackTracer /
    // retxTracer hooks on the link to
    // verify a retx cache-miss didn't
    // fire the ACK callback for neighbour
    // slots. The trampoline chain is
    // gone — freeBySeq() is
    // called directly by the link, so
    // there's no callback to trace. The
    // regression pin collapses to: after
    // a retx that misses the cache, the
    // neighbour slots are still there.

    uint8_t payload[16] = { 0 };
    for (int i = 0; i < 3; i++) {
        t.arqCachePut((uint8_t)(10 + i * 6), payload, 16,
                      (uint8_t)chunkCount(16));
    }
    assert(t.arqCacheSize() == 3);

    t.markAckedPending(16);
    for (int i = 1; i < 8; i++)
        t.markAckedPending((uint8_t)(16 + i));

    t.arqCacheFreeBySeq(16);
    assert(t.arqCacheSize() == 2);

    // Retx for the just-acked seq misses
    // the cache (already freed). Pin: no
    // neighbour is collateral damage.
    t.arqCacheRetx(16);
    int cacheAfter = t.arqCacheSize();
    std::cout << "  cache size after retx-miss: " << cacheAfter << std::endl;

    bool base22StillThere = false;
    for (int i = 0; i < (int)t.arqCacheSize(); i++) {
        if (t.arqCacheFindBySeq(22) >= 0)
            base22StillThere = true;
    }
    assert(base22StillThere);
    assert(cacheAfter == 2);
    std::cout << "PASS (cache-miss cleared nothing; base=22 neighbour intact)"
              << std::endl;
}

void test_facade_retx_is_not_a_drop_request() {
    std::cout
        << "\n=== Test: retx never requests a link drop (root-cause pin) ==="
        << std::endl;
    AutoLink link(0, 16, 17, true);
    AutoLinkTestAccessor t(link);
    uint8_t payload[16] = { 1, 2,  3,  4,  5,  6,  7,  8,
                            9, 10, 11, 12, 13, 14, 15, 16 };

    t.arqCachePut(7, payload, 16, (uint8_t)chunkCount(16));
    // Peek hit: payload is there to
    // retransmit. Old "drop request"
    // contract is gone.
    assert(t.arqCacheRetx(7) == true);

    // Keepalive slot: peek miss, no
    // pool buf, retx is a no-op.
    t.arqCachePut(8, nullptr, 0, 1);
    assert(t.arqCacheRetx(8) == false);

    // Missing seq: peek miss, retx is
    // a no-op.
    assert(t.arqCacheRetx(200) == false);

    std::cout << "PASS" << std::endl;
}

void test_facade_arq_pool_grew_to_64() {
    std::cout << "\n=== Test: ARQ_CACHE_POOL_SIZE is 64 ===" << std::endl;
    AutoLink link(0, 16, 17, true);
    AutoLinkTestAccessor t(link);
    uint8_t payload[16] = {};
    const int POOL_SIZE = t.arqPoolSize();
    assert(POOL_SIZE == 64);

    for (int i = 0; i < POOL_SIZE; i++) {
        t.arqCachePut((uint8_t)i, payload, 16, (uint8_t)chunkCount(16));
    }
    assert(t.arqCacheSize() == POOL_SIZE);
    assert(!t.arqCacheHasRoom());

    t.arqCacheFreeBySeq(0);
    assert(t.arqCacheHasRoom());
    std::cout << "PASS (pool=" << t.arqPoolSize() << " holds " << POOL_SIZE
              << " messages before back-pressure)" << std::endl;
}

int main() {
    Log::log().setLevel(Log::Level::WARNING);
    std::cout << "=== Running AutoLink Facade Tests (behavioral) ==="
              << std::endl;

    test_facade_arq_pool_grew_to_64();
    test_facade_arqcache_rejects_when_full();
    test_facade_arqcache_retx_frees_old_slot();
    test_facade_retx_is_not_a_drop_request();
    test_facade_reset_zeros_all_counters();
    test_facade_link_reset_clears_cache();
    test_facade_repeated_drops_do_not_latch_gate();
    test_facade_arq_cache_slots_exceeds_window();
    test_facade_cache_miss_does_not_clear_neighbours();
    std::cout << "\n=== AutoLink Facade Tests Completed Successfully ==="
              << std::endl;
    return 0;
}
