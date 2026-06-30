// ARQ payload cache: pool-backed, no
// malloc on the TX hot path. Pure
// storage; the link layer drives
// timing and retx writes. Extracted
// from AutoLink and behind IArqCache so the
// itest can use the production class
// directly.
#pragma once

#include "al/AutoLinkConfig.h"
#include "al/link/arq/IArqCache.h"
#include <cstdint>
#include <cstring>

#ifdef AUTOLINK_HOST_TEST
#    include <cassert>
#endif

namespace autolink {
class ArqCache : public IArqCache {
public:
    static constexpr int SLOTS = 256;    // 1:1 with cobsSeq space
    static constexpr int POOL_SIZE = 64; // 64 x 256B = 16 KB
    static constexpr int POOL_BUF_MAX = 256;

    // Window is owned by the flow
    // controller (Ping → AUTOLINK_ARQ_PIPELINE_WINDOW).
    // The cache validates the relationship
    // at construction: a cache that can't
    // hold a full window plus retx headroom
    // (POOL_SIZE >= 2*window) silently drops
    // sends and turns retx into a cache
    // miss. The runtime assert catches the
    // drift at the constructor call site —
    // the moment a developer widens the
    // pipeline without widening the pool.
    explicit ArqCache(int window = AUTOLINK_ARQ_PIPELINE_WINDOW);

    int window() const { return window_; }

    // Pipeline in-flight chunk budget
    // that Ping drives the link toward.
    // Re-exported as `WINDOW` on Ping.h.
    // The cache itself does NOT dictate
    // the pipeline depth; it only
    // validates its own pool can hold the
    // window the caller asked for.
    static_assert(POOL_BUF_MAX >= 256,
                  "POOL_BUF_MAX must cover a full MSG_HDR + MAX_CHUNK frame");

    struct Pending {
        uint16_t len = 0;
        uint8_t poolIdx = 0xFF;
        bool in_use = false;
    };

    // IArqCache
    bool hasRoom() const override;
    void insert(uint8_t seq, const uint8_t *payload, int payloadLen) override;
    void freeBySeq(uint8_t seq) override;
    bool peekForRetx(uint8_t seq, const uint8_t **outBuf,
                     int *outLen) const override;
    void clearAll() override;
    bool slotInUse(uint8_t seq) const override;
    int size() const override;

    // Per-slot inspection for tests.
    bool slotPeek(uint8_t seq, const uint8_t **outBuf, int *outLen) const;

    // Test-only fixtures: stamp the
    // pool bitmap / slot count into
    // known states. Used by
    // ArqCacheTest.cpp and the facade
    // overflow tests.
    void testFillPool();
    void testEmptyPool();
    void testFillSlots();

    // Test-only direct accessors used
    // by AutoLinkFacadeTest.cpp to
    // drive cache state without going
    // through the link layer.
    void testPut(uint8_t seq, const uint8_t *b, int len);
    bool testRetx(uint8_t seq, const uint8_t **outBuf, int *outLen) const;

#ifdef AUTOLINK_HOST_TEST
    void assertInvariants() const;
#else
    void assertInvariants() const {}
#endif

private:
    Pending pending_[SLOTS];
    int pendingCount_ = 0;
    uint8_t pool_[POOL_SIZE][POOL_BUF_MAX];
    bool poolUsed_[POOL_SIZE] = {};
    int poolFree_ = POOL_SIZE; // O(1) hasRoom; tracked alongside poolUsed_
    // Window the cache was constructed
    // against. POOL_SIZE must be >=
    // 2*window_ or insert() silently
    // drops. Validated in the ctor.
    int window_ = 0;
};
} // namespace autolink
