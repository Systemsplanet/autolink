
#include "ArqCache.h"
#include "al/util/log/Log.h"
#ifdef AUTOLINK_HOST_TEST
#    include <cassert>
#    include <iostream>
#endif

namespace autolink {

ArqCache::ArqCache(int window) : window_(window), windowMax_(window) {
#ifdef AUTOLINK_HOST_TEST
    assert(window > 0 && "ArqCache window must be positive");
    assert(POOL_SIZE >= window * 2 &&
           "ArqCache pool too small for pipeline window — bump POOL_SIZE or "
           "shrink the window");
#else
    if (window <= 0 || POOL_SIZE < window * 2) {
        Log::log().error(
            "ArqCache",
            "POOL_SIZE=%d < 2*window=%d (window=%d) — pipeline cache "
            "too small, retx will silently miss",
            POOL_SIZE, window * 2, window);
    }
#endif
}

void ArqCache::setWindow(int w) {
    if (w < 1)
        w = 1;
    if (w > windowMax_)
        w = windowMax_;
    window_ = w;
}
bool ArqCache::hasRoom() const {
    return pendingCount_ < SLOTS && poolFree_ > 0;
}

int ArqCache::freeRoom() const {
    int slots = SLOTS - pendingCount_;
    return slots < poolFree_ ? slots : poolFree_;
}

void ArqCache::insert(uint8_t seq, const uint8_t *payload, int payloadLen) {
    if (seq >= SLOTS)
        return;

    if (pending_[seq].in_use) {
        uint8_t oldPool = pending_[seq].poolIdx;
        if (oldPool < POOL_SIZE) {
            poolUsed_[oldPool] = false;
            if (poolFree_ < POOL_SIZE)
                poolFree_++;
        }
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

    if (poolFree_ <= 0) {
        Log::log().error(
            "ArqCache",
            "pool exhausted (size=%d) for cobsSeq=%u — slot skipped "
            "(retx for this chunk will be a cache miss)",
            POOL_SIZE, (unsigned)seq);
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
            "pool bookkeeping drift (poolFree_=%d but no free slot) for "
            "cobsSeq=%u — slot skipped",
            poolFree_, (unsigned)seq);
        return;
    }

    memcpy(pool_[freeIdx], payload, (size_t)payloadLen);
    pending_[seq].poolIdx = (uint8_t)freeIdx;
    pending_[seq].len = (uint16_t)payloadLen;
    pending_[seq].in_use = true;
    poolUsed_[freeIdx] = true;
    poolFree_--;
    pendingCount_++;
    assertInvariants();
}

void ArqCache::freeBySeq(uint8_t seq) { freeBySeq(seq, FreeCause::SingleAck); }

void ArqCache::freeBySeq(uint8_t seq, FreeCause cause) {
#ifndef AUTOLINK_TRACE_WIRE
    // AL97-7: cause is only consumed by the trace-gated causeName
    // switch below (the log line it feeds). Silence the unused-
    // parameter warning on a non-trace build without touching the
    // interface signature (IArqCache::freeBySeq requires it).
    (void)cause;
#endif
    if (seq >= SLOTS || !pending_[seq].in_use)
        return;
    uint8_t poolIdx = pending_[seq].poolIdx;
    if (poolIdx < POOL_SIZE) {
        poolUsed_[poolIdx] = false;
        if (poolFree_ < POOL_SIZE)
            poolFree_++;
    }
    pending_[seq].in_use = false;
    pending_[seq].len = 0;
    pending_[seq].poolIdx = 0xFF;
    if (pendingCount_ > 0)
        pendingCount_--;
    // Per-ACK free is a normal ARQ event but per-ACK logging at
    // info level is exactly the per-ACK flood the verbose demotion
    // was written to prevent. Debug, not info. The `seq=` /
    // `pending=` field-name form (instead of `cobsSeq=%u
    // (pending=%d)`) keeps the line parseable by simple log
    // greps — operators can `awk '$2 == "seq=12"'` without
    // unwrapping parens. The cause tag distinguishes a
    // single-seq ACK from a cumulative walk / NAK-cumulative /
    // honest-drop — the same wire line would otherwise look
    // identical in the log and a real drop was indistinguishable
    // from a healthy cumulative free. Pinned by
    // ArqCacheFreeCauseTest.
    // AL97-7: causeName is only consumed by the AUTOLINK_TRACE_WIRE
    // verbose line below; computing it unconditionally left it
    // unused (and warned on) once that line was gated. See
    // AckPathNotVerboseByDefaultTest.
#ifdef AUTOLINK_TRACE_WIRE
    const char *causeName = "SingleAck";
    switch (cause) {
    case FreeCause::SingleAck:
        causeName = "SingleAck";
        break;
    case FreeCause::CumulativeBackfill:
        causeName = "CumulativeBackfill";
        break;
    case FreeCause::NakCumulative:
        causeName = "NakCumulative";
        break;
    case FreeCause::HonestDrop:
        causeName = "HonestDrop";
        break;
    case FreeCause::Reset:
        causeName = "Reset";
        break;
    }
#endif
    // AL97-7: same AUTOLINK_TRACE_WIRE gate as the wire-level ACK
    // trace this line is a per-freed-chunk companion to (a
    // cumulative-walk ACK frees multiple slots, so this can fire
    // more often than the ACK line itself). HonestDrop/Reset causes
    // are not lost by gating this off — droppedChunksOnReset
    // (LinkCore.cpp) already logs and counts reset-driven wipes at
    // warning level, independently of this per-slot line. Pinned by
    // AckPathNotVerboseByDefaultTest.
#ifdef AUTOLINK_TRACE_WIRE
    Log::log().debug("ArqCache", "free seq=%u pending=%d cause=%s",
                     (unsigned)seq, pendingCount_, causeName);
#endif
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
    poolFree_ = POOL_SIZE;
    Log::log().info(
        "ArqCache",
        "cleared (link reset): %d slot(s) and %d pool buffer(s) freed", SLOTS,
        POOL_SIZE);
}

int ArqCache::size() const { return pendingCount_; }

bool ArqCache::slotInUse(uint8_t seq) const {
    return seq < SLOTS && pending_[seq].in_use;
}

void ArqCache::testFillPool() {
    for (int i = 0; i < POOL_SIZE; i++)
        poolUsed_[i] = true;
    poolFree_ = 0;
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

    assert(poolFree_ == POOL_SIZE - poolUsedCount &&
           "poolFree_ drift from poolUsed_ scan");
    assert(poolFree_ >= 0);
    assert(poolFree_ <= POOL_SIZE);
}
#endif

} // namespace autolink
