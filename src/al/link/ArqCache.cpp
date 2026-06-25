// ARQ payload cache. Pure storage; no
// I/O, no link callbacks.
#include "ArqCache.h"
#include "al/util/Log.h"
#ifdef AUTOLINK_HOST_TEST
#    include <cassert>
#    include <iostream>
#endif

namespace autolink {
bool ArqCache::hasRoom() const {
    if (pendingCount_ >= SLOTS)
        return false;
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!poolUsed_[i])
            return true;
    }
    return false;
}

void ArqCache::insert(uint8_t seq, const uint8_t *payload, int payloadLen) {
    if (seq >= SLOTS)
        return;

    if (pending_[seq].in_use) {
        uint8_t oldPool = pending_[seq].poolIdx;
        if (oldPool < POOL_SIZE)
            poolUsed_[oldPool] = false;
        pending_[seq].poolIdx = 0xFF;
        pending_[seq].len = 0;
        if (pendingCount_ > 0)
            pendingCount_--;
    }

    if (payloadLen <= 0 || !payload) {
        pending_[seq].in_use = true;
        pending_[seq].len = 0;
        pending_[seq].poolIdx = 0xFF;
        pendingCount_++;
        assertInvariants();
        return;
    }
    if (payloadLen > POOL_BUF_MAX) {
        Log::log().error(
            "ArqCache", "chunk too large for pool buffer: %d > %d (cobsSeq=%u)",
            payloadLen, POOL_BUF_MAX, (unsigned)seq);
        return;
    }

    int freeIdx = -1;
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!poolUsed_[i]) {
            freeIdx = i;
            break;
        }
    }
    if (freeIdx < 0) {
        Log::log().error(
            "ArqCache",
            "pool exhausted (size=%d) for cobsSeq=%u — slot skipped "
            "(retx for this chunk will be a cache miss)",
            POOL_SIZE, (unsigned)seq);
        return;
    }

    memcpy(pool_[freeIdx], payload, (size_t)payloadLen);
    pending_[seq].poolIdx = (uint8_t)freeIdx;
    pending_[seq].len = (uint16_t)payloadLen;
    pending_[seq].in_use = true;
    poolUsed_[freeIdx] = true;
    pendingCount_++;
    assertInvariants();
}

void ArqCache::freeBySeq(uint8_t seq) {
    if (seq >= SLOTS || !pending_[seq].in_use)
        return;
    uint8_t poolIdx = pending_[seq].poolIdx;
    if (poolIdx < POOL_SIZE)
        poolUsed_[poolIdx] = false;
    pending_[seq].in_use = false;
    pending_[seq].len = 0;
    pending_[seq].poolIdx = 0xFF;
    if (pendingCount_ > 0)
        pendingCount_--;
}

bool ArqCache::peekForRetx(uint8_t seq, const uint8_t **outBuf,
                           int *outLen) const {
    if (seq >= SLOTS || !pending_[seq].in_use) {
        if (outBuf)
            *outBuf = nullptr;
        if (outLen)
            *outLen = 0;
        return false;
    }
    uint8_t poolIdx = pending_[seq].poolIdx;
    if (poolIdx >= POOL_SIZE || pending_[seq].len == 0) {
        if (outBuf)
            *outBuf = nullptr;
        if (outLen)
            *outLen = 0;
        return false;
    }
    if (outBuf)
        *outBuf = pool_[poolIdx];
    if (outLen)
        *outLen = pending_[seq].len;
    return true;
}

void ArqCache::clearAll() {
    for (int i = 0; i < SLOTS; i++) {
        pending_[i].in_use = false;
        pending_[i].len = 0;
        pending_[i].poolIdx = 0xFF;
    }
    for (int i = 0; i < POOL_SIZE; i++)
        poolUsed_[i] = false;
    pendingCount_ = 0;
    Log::log().info(
        "ArqCache",
        "cleared (link reset): %d slot(s) and %d pool buffer(s) freed", SLOTS,
        POOL_SIZE);
}

int ArqCache::size() const { return pendingCount_; }

bool ArqCache::slotInUse(uint8_t seq) const {
    return seq < SLOTS && pending_[seq].in_use;
}

bool ArqCache::slotPeek(uint8_t seq, const uint8_t **outBuf,
                        int *outLen) const {
    return peekForRetx(seq, outBuf, outLen);
}

void ArqCache::testFillPool() {
    for (int i = 0; i < POOL_SIZE; i++)
        poolUsed_[i] = true;
}

void ArqCache::testEmptyPool() {
    for (int i = 0; i < POOL_SIZE; i++)
        poolUsed_[i] = false;
}

void ArqCache::testFillSlots() { pendingCount_ = SLOTS; }

void ArqCache::testPut(uint8_t seq, const uint8_t *b, int len) {
    insert(seq, b, len);
}

bool ArqCache::testRetx(uint8_t seq, const uint8_t **outBuf,
                        int *outLen) const {
    return peekForRetx(seq, outBuf, outLen);
}

#ifdef AUTOLINK_HOST_TEST
void ArqCache::assertInvariants() const {
    int inUse = 0;
    int inUseWithPool = 0;
    int poolUsedCount = 0;
    for (int i = 0; i < SLOTS; i++) {
        const Pending &p = pending_[i];
        if (!p.in_use) {
            assert(p.poolIdx == 0xFF && "in_use=false but poolIdx != 0xFF");
            assert(p.len == 0);
            continue;
        }
        inUse++;
        if (p.poolIdx != 0xFF)
            inUseWithPool++;
        assert(p.len <= POOL_BUF_MAX);
    }
    for (int i = 0; i < POOL_SIZE; i++) {
        if (poolUsed_[i])
            poolUsedCount++;
    }
    assert(inUse == pendingCount_ &&
           "pendingCount_ does not match in_use slot count");
    assert(inUseWithPool == poolUsedCount &&
           "pool-used count does not match pending slots with a pool buffer");
    assert(pendingCount_ >= 0);
    assert(pendingCount_ <= SLOTS);
    assert(poolUsedCount >= 0);
    assert(poolUsedCount <= POOL_SIZE);
}
#endif

} // namespace autolink
