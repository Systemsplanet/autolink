
#pragma once

#include "al/link/LinkWire.h"
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
    static constexpr int SLOTS = 256;
    static constexpr int POOL_SIZE = 64;
    static constexpr int POOL_BUF_MAX = 256;

    explicit ArqCache(int window = AUTOLINK_ARQ_PIPELINE_WINDOW);

    int window() const override { return window_; }
    void setWindow(int w) override;

    static_assert(POOL_BUF_MAX >= 256,
                  "POOL_BUF_MAX must cover a full MSG_HDR + MAX_CHUNK frame");

    struct Pending {
        uint16_t len = 0;
        uint8_t poolIdx = 0xFF;
        bool in_use = false;
    };

    bool hasRoom() const override;
    int freeRoom() const override;
    void insert(uint8_t seq, const uint8_t *payload, int payloadLen) override;
    void freeBySeq(uint8_t seq) override;
    // Caller-tagged variant. Default-arg keeps the
    // existing freeBySeq(uint8_t) override signature
    // stable for non-Link callers (the IArqCache
    // contract). FreeCause is declared on IArqCache
    // so the virtual override is well-formed.
    void freeBySeq(uint8_t seq, FreeCause cause) override;
    bool peekForRetx(uint8_t seq, const uint8_t **outBuf,
                     int *outLen) const override;
    void clearAll() override;
    bool slotInUse(uint8_t seq) const override;
    int size() const override;

    void testFillPool();
    void testFillSlots();

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
    int poolFree_ = POOL_SIZE;

    int window_ = 0;
    // Ceiling setWindow() clamps against — the window the cache
    // was constructed with. setWindow() only ever shrinks toward
    // this, never past it, so a later call with a larger ring
    // reading (e.g. a fresh begin() after freeing heap) can
    // recover back up to the constructed compile-time default
    // rather than ratcheting down permanently.
    int windowMax_ = 0;
};
} // namespace autolink
