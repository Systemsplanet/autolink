// ARQ payload cache: pool-backed, no malloc-per-chunk.
// Returns false on retx/ack on every handled path — a
// successful retransmit is the opposite of a reason to
// drop the link.
#include "AutoLink.h"
#include <cstdio>
#ifdef AUTOLINK_HOST_TEST
#    include <cassert>
#    include <iostream>
#endif

namespace autolink
{
bool AutoLink::arqAckHookTrampoline(uint8_t ackedSeq,
                                    void *ctx)
{
    AutoLink *self = static_cast<AutoLink *>(ctx);
    if (self->arqCache_findBySeq(ackedSeq) < 0)
        return false;
    self->arqCache_freeBySeq(ackedSeq);
    return false;
}

bool AutoLink::arqRetxHookTrampoline(uint8_t retxSeq,
                                     void *ctx)
{
    return static_cast<AutoLink *>(ctx)->arqCache_retx(
        retxSeq);
}

bool AutoLink::arqCache_hasRoom()
{
    if (pendingCount_ >= ARQ_CACHE_SLOTS)
        return false;
    for (int i = 0; i < ARQ_CACHE_POOL_SIZE; i++) {
        if (!arqPoolUsed_[i])
            return true;
    }
    return false;
}

void AutoLink::arqCache_insert_unlocked(
    uint8_t seq, const uint8_t *payload,
    int payloadLen, uint8_t chunkCount)
{
    (void)chunkCount;
    if (seq >= ARQ_CACHE_SLOTS)
        return;


    if (pending_[seq].in_use) {
        uint8_t oldPool = pending_[seq].poolIdx;
        if (oldPool < ARQ_CACHE_POOL_SIZE)
            arqPoolUsed_[oldPool] = false;
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
        assertCacheInvariants();
        return;
    }
    if (payloadLen > ARQ_POOL_BUF_MAX) {
        Log::log().error(
            "AutoLink",
            "ARQ cache chunk too large for pool "
            "buffer: %d > %d (cobsSeq=%u)",
            payloadLen, ARQ_POOL_BUF_MAX,
            (unsigned)seq);
        return;
    }


    int freeIdx = -1;
    for (int i = 0; i < ARQ_CACHE_POOL_SIZE; i++) {
        if (!arqPoolUsed_[i]) {
            freeIdx = i;
            break;
        }
    }
    if (freeIdx < 0) {
        Log::log().error(
            "AutoLink",
            "ARQ cache pool exhausted (size=%d) for "
            "cobsSeq=%u — slot skipped "
            "(retx for this chunk will be a cache "
            "miss)",
            ARQ_CACHE_POOL_SIZE, (unsigned)seq);
        return;
    }

    memcpy(arqPool_[freeIdx], payload,
           (size_t)payloadLen);
    pending_[seq].poolIdx = (uint8_t)freeIdx;
    pending_[seq].len = (uint16_t)payloadLen;
    pending_[seq].in_use = true;
    arqPoolUsed_[freeIdx] = true;
    pendingCount_++;
    assertCacheInvariants();
}

int AutoLink::arqCache_findBySeq(uint8_t seq)
{
    if (seq >= ARQ_CACHE_SLOTS ||
        !pending_[seq].in_use)
        return -1;
    return (int)seq;
}

void AutoLink::arqCache_freeBySeq(uint8_t seq)
{
    int idx = arqCache_findBySeq(seq);
    if (idx < 0)
        return;
    uint8_t poolIdx = pending_[idx].poolIdx;
    if (poolIdx < ARQ_CACHE_POOL_SIZE)
        arqPoolUsed_[poolIdx] = false;
    pending_[idx].in_use = false;
    pending_[idx].len = 0;
    pending_[idx].poolIdx = 0xFF;
    if (pendingCount_ > 0)
        pendingCount_--;
}

bool AutoLink::arqCache_retx(uint8_t seq)
{
    int idx = arqCache_findBySeq(seq);
    if (idx < 0) {
        Log::log().info(
            "AutoLink",
            "ARQ retransmit cache miss at cobsSeq=%u "
            "(chunk already "
            "delivered); pending bit left to time out",
            (unsigned)seq);
        return false;
    }
    uint8_t poolIdx = pending_[idx].poolIdx;
    int n = pending_[idx].len;
    if (poolIdx >= ARQ_CACHE_POOL_SIZE || n == 0) {
        Log::log().info(
            "AutoLink",
            "ARQ retransmit cobsSeq=%u (keepalive, no "
            "pool buf) — verbatim 0 bytes",
            (unsigned)seq);
        if (link)
            link->resendCobsFrame_unlocked(seq,
                                           nullptr, 0);
        return false;
    }
    Log::log().warning(
        "AutoLink",
        "ARQ retransmit cobsSeq=%u (%d bytes, slot=%d "
        "pool=%u) — verbatim",
        (unsigned)seq, n, (int)idx, (unsigned)poolIdx);
    if (link) {
        link->resendCobsFrame_unlocked(
            seq, arqPool_[poolIdx], n);
    }
    return false;
}

void AutoLink::arqCache_clearAll()
{
    for (int i = 0; i < ARQ_CACHE_SLOTS; i++) {
        pending_[i].in_use = false;
        pending_[i].len = 0;
        pending_[i].poolIdx = 0xFF;
    }
    for (int i = 0; i < ARQ_CACHE_POOL_SIZE; i++)
        arqPoolUsed_[i] = false;
    pendingCount_ = 0;
    Log::log().info(
        "AutoLink",
        "ARQ cache cleared (link reset): %d slot(s) "
        "and %d pool buffer(s) freed",
        ARQ_CACHE_SLOTS, ARQ_CACHE_POOL_SIZE);
}

#ifndef AUTOLINK_HOST_TEST
inline void AutoLink::assertCacheInvariants() const {}
#endif
#ifdef AUTOLINK_HOST_TEST
void AutoLink::assertCacheInvariants() const
{
    int inUse = 0;
    int inUseWithPool = 0;
    int poolUsedCount = 0;
    for (int i = 0; i < ARQ_CACHE_SLOTS; i++) {
        const Pending &p = pending_[i];
        if (!p.in_use) {
            assert(p.poolIdx == 0xFF &&
                   "in_use=false but poolIdx != 0xFF");
            assert(p.len == 0);
            continue;
        }
        inUse++;
        if (p.poolIdx != 0xFF)
            inUseWithPool++;
        assert(p.len <= ARQ_POOL_BUF_MAX);
    }
    for (int i = 0; i < ARQ_CACHE_POOL_SIZE; i++) {
        if (arqPoolUsed_[i])
            poolUsedCount++;
    }
    assert(inUse == pendingCount_ &&
           "pendingCount_ does not match in_use slot "
           "count");
    assert(inUseWithPool == poolUsedCount &&
           "pool-used count does not match pending "
           "slots with a pool buffer");
    assert(pendingCount_ >= 0);
    assert(pendingCount_ <= ARQ_CACHE_SLOTS);
    assert(poolUsedCount >= 0);
    assert(poolUsedCount <= ARQ_CACHE_POOL_SIZE);
}
#endif

void AutoLink::linkResetHookTrampoline(void *ctx)
{
    if (auto *self = static_cast<AutoLink *>(ctx))
        self->arqCache_clearAll();
}

bool AutoLink::arqCacheHasRoomTrampoline(void *ctx)
{
    auto *self = static_cast<AutoLink *>(ctx);
    return self &&
        self->pendingCount_ < ARQ_CACHE_SLOTS;
}

void AutoLink::arqCacheInsertTrampoline(
    uint8_t seq, const uint8_t *payload,
    int payloadLen, uint8_t chunkCount, void *ctx)
{
    if (auto *self = static_cast<AutoLink *>(ctx)) {
        self->arqCache_insert_unlocked(
            seq, payload, payloadLen, chunkCount);
    }
}

void AutoLink::arqCacheClearAllTrampoline(void *ctx)
{
    if (auto *self = static_cast<AutoLink *>(ctx))
        self->arqCache_clearAll();
}

} // namespace autolink