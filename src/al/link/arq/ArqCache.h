// ARQ payload cache: pool-backed, no
// malloc on the TX hot path. Pure
// storage; the link layer drives
// timing and retx writes. Extracted
// from AutoLink and behind IArqCache so the
// itest can use the production class
// directly.
#pragma once

#include "al/link/arq/IArqCache.h"
#include <cstdint>
#include <cstring>

namespace autolink {
class ArqCache : public IArqCache {
public:
    static constexpr int SLOTS = 256;    // 1:1 with cobsSeq space
    static constexpr int POOL_SIZE = 64; // 64 x 256B = 16 KB
    static constexpr int POOL_BUF_MAX = 256;

    // Pipeline in-flight chunk budget
    // shared with Ping. Pool must hold
    // a full window plus headroom for
    // unacked slots still held by retx,
    // or insert() silently drops and
    // retx becomes a cache miss → link
    // reset. Compile-time guard against
    // drift between the two constants.
    static constexpr int WINDOW = 32;
    static_assert(POOL_SIZE >= WINDOW * 2,
                  "ArqCache pool too small for pipeline window");

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
};
} // namespace autolink
