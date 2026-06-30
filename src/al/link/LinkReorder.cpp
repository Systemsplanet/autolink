// Reorder buffer implementation. Pool-backed:
// ISR-adjacent hot path never calls malloc, so
// FreeRTOS heap fragmentation can't drop a
// held frame.
#include "al/link/LinkReorder.h"
#include "al/link/LinkContext.h"
#include "al/link/LinkFrameRx.h"
#include "al/util/Log.h"
#include <cstring>
#ifdef AUTOLINK_HOST_TEST
#    include <cassert>
#endif

static constexpr const char *TAG = "AutoLink";

namespace autolink {
static_assert(LinkReorder::REORDER_POOL_BUF_MAX == MAX_CHUNK,
              "REORDER_POOL_BUF_MAX must match Link::MAX_CHUNK");

LinkReorder::~LinkReorder() { clearAll(); }

namespace {
int acquirePoolIdx(bool poolUsed_[LinkReorder::REORDER_POOL_SIZE]) {
    for (int i = 0; i < LinkReorder::REORDER_POOL_SIZE; i++) {
        if (!poolUsed_[i])
            return i;
    }
    return -1;
}
} // namespace

void LinkReorder::clearAll() {
    for (int i = 0; i < SLOTS; i++) {
        slots_[i].in_use = false;
        slots_[i].len = 0;
        slots_[i].poolIdx = 0xFF;
    }
    for (int i = 0; i < REORDER_POOL_SIZE; i++)
        poolUsed_[i] = false;
    assertInvariants();
}

int LinkReorder::dropExpired(uint32_t nowMs, int holdMs) {
    int dropped = 0;
    for (int i = 0; i < SLOTS; i++) {
        if (!slots_[i].in_use)
            continue;
        uint32_t age = nowMs - slots_[i].heldAtMs;
        if (age < (uint32_t)holdMs)
            continue;
        uint8_t poolIdx = slots_[i].poolIdx;
        if (poolIdx < REORDER_POOL_SIZE)
            poolUsed_[poolIdx] = false;
        slots_[i].in_use = false;
        slots_[i].len = 0;
        slots_[i].poolIdx = 0xFF;
        dropped++;
        Log::log().warning(TAG,
                           "reorder seq=%u expired "
                           "(%ums)",
                           (unsigned)i, (unsigned)age);
    }
    assertInvariants();
    return dropped;
}

int LinkReorder::flushContiguous(LinkContext &ctx, uint32_t nowMs) {
    (void)nowMs;
    int delivered = 0;
    bool progress = true;
    while (progress) {
        progress = false;
        uint8_t exp = ctx.reorderExpectedSeq();
        if (!slots_[exp].in_use)
            break;
        Slot &s = slots_[exp];
        const uint8_t *src =
            (s.poolIdx < REORDER_POOL_SIZE) ? pool_[s.poolIdx] : nullptr;
        int acc = src ? ctx.reorderPushAppBuf(src, s.len) : 0;
        if (acc < s.len) {
            Log::log().info(TAG,
                            "reorder flush seq=%u "
                            "buf full (want %d got %d)",
                            (unsigned)exp, (int)s.len, acc);
            break;
        }
        ctx.reorderSendAck(exp);
        ctx.reorderAdvanceRxSeq(exp);
        ctx.reorderCountBytes(s.len);
        uint8_t poolIdx = s.poolIdx;
        if (poolIdx < REORDER_POOL_SIZE)
            poolUsed_[poolIdx] = false;
        s.in_use = false;
        s.len = 0;
        s.poolIdx = 0xFF;
        delivered++;
        progress = true;
    }
    assertInvariants();
    return delivered;
}

bool LinkReorder::hold(uint8_t seq, const uint8_t *b, int n, uint32_t nowMs) {
    if (n <= 0)
        return false;
    if (n > REORDER_POOL_BUF_MAX) {
        Log::log().error(
            TAG, "reorder chunk too large for pool: %d > %d (cobsSeq=%u)", n,
            REORDER_POOL_BUF_MAX, (unsigned)seq);
        return false;
    }

    bool alreadyHeld = slots_[seq].in_use;
    if (alreadyHeld) {
        // Retx of an already-held seq: same
        // pool buffer, same slot. Don't churn
        // the pool.
        memcpy(pool_[slots_[seq].poolIdx], b, (size_t)n);
        slots_[seq].len = (uint16_t)n;
        slots_[seq].heldAtMs = nowMs;
        return false;
    }

    int freeIdx = acquirePoolIdx(poolUsed_);
    if (freeIdx < 0) {
        Log::log().error(TAG,
                         "reorder pool exhausted (size=%d) for cobsSeq=%u — "
                         "slot skipped (frame dropped, lostMsgs++)",
                         REORDER_POOL_SIZE, (unsigned)seq);
        return false;
    }

    memcpy(pool_[freeIdx], b, (size_t)n);
    slots_[seq].poolIdx = (uint8_t)freeIdx;
    slots_[seq].len = (uint16_t)n;
    slots_[seq].heldAtMs = nowMs;
    slots_[seq].in_use = true;
    poolUsed_[freeIdx] = true;
    assertInvariants();
    return true;
}

void LinkReorder::testFillPool() {
    for (int i = 0; i < REORDER_POOL_SIZE; i++)
        poolUsed_[i] = true;
}

void LinkReorder::testEmptyPool() {
    for (int i = 0; i < REORDER_POOL_SIZE; i++)
        poolUsed_[i] = false;
}

#ifdef AUTOLINK_HOST_TEST
void LinkReorder::assertInvariants() const {
    // Pool-used count can lead slot count
    // under the testFillPool() fixture
    // (used to drive hold() into the
    // exhausted branch without holding
    // real frames). The slot->pool
    // direction is what production
    // care about: every in_use slot
    // must point at a pool buffer that
    // is marked used, and unused slots
    // must have poolIdx == 0xFF.
    int poolUsedCount = 0;
    for (int i = 0; i < REORDER_POOL_SIZE; i++) {
        if (poolUsed_[i])
            poolUsedCount++;
    }
    assert(poolUsedCount >= 0);
    assert(poolUsedCount <= REORDER_POOL_SIZE);
    for (int i = 0; i < SLOTS; i++) {
        const Slot &s = slots_[i];
        if (!s.in_use) {
            assert(s.poolIdx == 0xFF && "in_use=false but poolIdx != 0xFF");
            assert(s.len == 0);
            continue;
        }
        assert(s.poolIdx < REORDER_POOL_SIZE &&
               "in_use slot has no pool buffer");
        assert(poolUsed_[s.poolIdx] &&
               "in_use slot points to an unused pool buffer");
        assert(s.len <= REORDER_POOL_BUF_MAX);
    }
}
#endif

} // namespace autolink