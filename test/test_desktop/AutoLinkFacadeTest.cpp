// AutoLink facade contract: ARQ cache hooks, ack/retx
// wiring, drop-after-reset callback.


#ifndef AUTOLINK_HOST_TEST
#    error \
        "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include "al/hal/IHal.h"
#include "al/link/Link.h"
#include "al/util/Log.h"
#include "al/util/UtilBlink.h"
#include "AutoLink.h"

#include <iostream>
#include <cassert>
#include <string>
#include <vector>

using namespace autolink;

static int chunkCount(int len)
{
    return 1 + (len + 250 - 1) / 250;
}

void test_facade_arqcache_rejects_when_full()
{
    std::cout << "\n=== Test: ARQ cache gate "
                 "(pool-backed, size=16) ==="
              << std::endl;
    AutoLink link(0, 16, 17, true);

    uint8_t payload[32] = { 0 };
    int filled = 0;
    for (int i = 0; i < 16; i++) {
        link.test_arqCache_put(
            (uint8_t)i, payload, 32,
            (uint8_t)chunkCount(32));
        filled++;
    }
    assert(filled == 16);
    assert(link.arqCacheSizeForTest() == 16);


    link.test_arqCache_put(16, payload, 32,
                           (uint8_t)chunkCount(32));
    assert(link.arqCacheSizeForTest() == 16);
    assert(!link.test_arqCache_hasRoom());

    link.test_arqCache_freeBySeq(0);
    assert(link.test_arqCache_hasRoom());

    link.test_arqCache_put(16, payload, 32,
                           (uint8_t)chunkCount(32));
    assert(link.arqCacheSizeForTest() == 16);
    std::cout << "PASS" << std::endl;
}

void test_facade_arqcache_retx_frees_old_slot()
{
    std::cout
        << "\n=== Test: arqCache retx keeps the slot "
           "alive; ack frees it (the fix) ==="
        << std::endl;
    AutoLink link(0, 16, 17, true);
    uint8_t payload[16] = { 1,  2,  3,  4,  5,  6,
                            7,  8,  9,  10, 11, 12,
                            13, 14, 15, 16 };
    link.test_arqCache_put(7, payload, 16,
                           (uint8_t)chunkCount(16));
    assert(link.arqCacheSizeForTest() == 1);


    bool dropReq = link.test_arqCache_retx(7);
    assert(!dropReq);
    assert(link.arqCacheSizeForTest() == 1);


    link.test_arqCache_freeBySeq(7);
    assert(link.arqCacheSizeForTest() == 0);

    std::cout << "PASS" << std::endl;
}

void test_facade_reset_zeros_all_counters()
{
    std::cout << "\n=== Test: AutoLink facade resets "
                 "all counters ==="
              << std::endl;
    AutoLink link(0, 16, 17, true);

    uint8_t payload[32] = { 0 };
    for (int i = 0; i < 5; i++) {
        link.test_arqCache_put(
            (uint8_t)i, payload, 32,
            (uint8_t)chunkCount(32));
    }
    assert(link.arqCacheSizeForTest() == 5);

    Link *lk = link.linkForTest();
    assert(lk != nullptr);
    lk->resetStats();
    lk->resetErrors();
    lk->resetDiag();
    Stats s;
    lk->getStats(s);
    if (s.tx != 0 || s.rx != 0 || s.discCount != 0 ||
        s.frameErrs != 0) {
        std::cerr << "\nLink stats should be 0 after "
                     "reset, got tx="
                  << (long long)s.tx
                  << " rx=" << (long long)s.rx
                  << " disc=" << (long long)s.discCount
                  << " frameErrs="
                  << (long long)s.frameErrs
                  << std::endl;
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

void test_facade_link_reset_clears_cache()
{
    std::cout << "\n=== Test: link drop clears the "
                 "facade cache (killer) ==="
              << std::endl;
    AutoLink link(0, 16, 17, true);
    uint8_t payload[64] = {};


    for (int i = 0; i < 24; i++) {
        link.test_arqCache_put(
            (uint8_t)i, payload, 64,
            (uint8_t)chunkCount(64));
    }
    int beforeDrop = link.arqCacheSizeForTest();


    if (beforeDrop != 16) {
        std::cerr << "\nfixture: cache should be 16 "
                     "(pool full), got "
                  << beforeDrop << std::endl;
        assert(false);
    }


    AutoLink::test_linkResetHookTrampoline(&link);
    int afterDrop = link.arqCacheSizeForTest();
    if (afterDrop != 0) {
        std::cerr << "\nFAIL: cache should be 0 after "
                     "link drop, got "
                  << afterDrop
                  << " — the facade cache is not "
                     "being cleared on link drop,"
                  << " the gate will latch after "
                     "enough drops."
                  << std::endl;
        assert(false);
    }
    std::cout << "  before drop: " << beforeDrop
              << " slots, after drop: " << afterDrop
              << " slots" << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_facade_repeated_drops_do_not_latch_gate()
{
    std::cout << "\n=== Test: N drops don't latch the "
                 "cache-full gate (saturation) ==="
              << std::endl;
    AutoLink link(0, 16, 17, true);
    uint8_t payload[32] = {};
    for (int cycle = 0; cycle < 5; cycle++) {
        for (int i = 0; i < 20; i++) {
            link.test_arqCache_put(
                (uint8_t)((cycle * 32 + i) & 0xFF),
                payload, 32, (uint8_t)chunkCount(32));
        }
        int beforeDrop = link.arqCacheSizeForTest();
        if (beforeDrop != 16) {
            std::cerr << "\ncycle " << cycle
                      << ": cache should be 16 before "
                         "drop (pool full), got "
                      << beforeDrop << std::endl;
            assert(false);
        }

        AutoLink::test_linkResetHookTrampoline(&link);
        int afterDrop = link.arqCacheSizeForTest();
        if (afterDrop != 0) {
            std::cerr << "\nFAIL cycle " << cycle
                      << ": cache should be 0 after "
                         "drop, got "
                      << afterDrop
                      << " — pendingCount_ ratchets "
                         "up across drops, "
                      << "the gate will latch after "
                         "enough drops."
                      << std::endl;
            assert(false);
        }
    }
    std::cout << "  5 drop cycles, cache returned to "
                 "0 after each"
              << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_facade_arq_cache_slots_exceeds_window()
{
    std::cout << "\n=== Test: ARQ_CACHE_SLOTS > "
                 "WINDOW (Bug 4) ==="
              << std::endl;
    int cache_slots = AutoLink::ARQ_CACHE_SLOTS_PUBLIC;
    int window = 32;
    if (cache_slots <= window) {
        std::cerr << "\nFAIL: ARQ_CACHE_SLOTS="
                  << cache_slots
                  << " must exceed WINDOW=" << window
                  << ". With zero margin, a single "
                     "mis-keyed/leaked slot "
                  << "ratchets the cache to full and "
                     "the gate latches."
                  << std::endl;
        assert(false);
    }
    std::cout << "  ARQ_CACHE_SLOTS=" << cache_slots
              << " > WINDOW=" << window << " ✓"
              << std::endl;
    std::cout << "PASS" << std::endl;
}

void test_facade_cache_miss_does_not_clear_neighbours()
{
    std::cout
        << "\n=== Test: cache-miss cleanup never "
           "crosses message boundaries (the fix) ==="
        << std::endl;
    AutoLink link(0, 16, 17, true);


    int ackCalls = 0;
    std::vector<uint8_t> ackBases;
    auto ackTracer = [](uint8_t base,
                        void *ctx) -> bool {
        auto *p = static_cast<std::pair<
            int *, std::vector<uint8_t> *> *>(ctx);
        (*p->first)++;
        p->second->push_back(base);
        return false;
    };
    auto retxTracer = [](uint8_t, void *) -> bool {
        return false;
    };
    auto ctx = std::make_pair(&ackCalls, &ackBases);
    link.linkForTest()->setArqHooks(ackTracer,
                                    retxTracer, &ctx);


    uint8_t payload[16] = { 0 };
    for (int i = 0; i < 3; i++) {
        link.test_arqCache_put(
            (uint8_t)(10 + i * 6), payload, 16,
            (uint8_t)chunkCount(16));
    }
    assert(link.arqCacheSizeForTest() == 3);


    link.test_markAckedPending(16);
    for (int i = 1; i < 8; i++)
        link.test_markAckedPending((uint8_t)(16 + i));


    link.test_arqCache_freeBySeq(16);
    assert(link.arqCacheSizeForTest() == 2);
    assert(ackCalls == 0);


    link.test_arqCache_retx(16);


    int cacheAfter = link.arqCacheSizeForTest();
    std::cout << "  miss-cleanup fired ackTracer "
              << ackCalls << " times with bases: ";
    for (auto b : ackBases)
        std::cout << (int)b << " ";
    std::cout << "\n  cache size after miss: "
              << cacheAfter << std::endl;


    assert(ackCalls == 0);


    bool base22StillThere = false;
    for (int i = 0;
         i < (int)link.arqCacheSizeForTest(); i++) {
        if (link.test_arqCache_findBySeq(22) >= 0)
            base22StillThere = true;
    }
    assert(base22StillThere);
    assert(cacheAfter == 2);
    std::cout << "PASS (cache-miss cleared nothing; "
                 "base=22 neighbour intact)"
              << std::endl;
}

void test_facade_retx_is_not_a_drop_request()
{
    std::cout << "\n=== Test: retx never requests a "
                 "link drop (root-cause pin) ==="
              << std::endl;
    AutoLink link(0, 16, 17, true);
    uint8_t payload[16] = { 1,  2,  3,  4,  5,  6,
                            7,  8,  9,  10, 11, 12,
                            13, 14, 15, 16 };


    link.test_arqCache_put(7, payload, 16,
                           (uint8_t)chunkCount(16));
    assert(link.test_arqCache_retx(7) == false);


    link.test_arqCache_put(8, nullptr, 0, 1);
    assert(link.test_arqCache_retx(8) == false);


    assert(link.test_arqCache_retx(200) == false);

    std::cout << "PASS" << std::endl;
}

int main()
{
    Log::log().setLevel(Log::Level::WARNING);
    std::cout << "=== Running AutoLink Facade Tests "
                 "(behavioral) ==="
              << std::endl;


    test_facade_arqcache_rejects_when_full();
    test_facade_arqcache_retx_frees_old_slot();
    test_facade_retx_is_not_a_drop_request();
    test_facade_reset_zeros_all_counters();
    test_facade_link_reset_clears_cache();
    test_facade_repeated_drops_do_not_latch_gate();
    test_facade_arq_cache_slots_exceeds_window();
    test_facade_cache_miss_does_not_clear_neighbours();
    std::cout << "\n=== AutoLink Facade Tests "
                 "Completed Successfully ==="
              << std::endl;
    return 0;
}
