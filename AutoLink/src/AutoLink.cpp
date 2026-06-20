// AutoLink.cpp — facade implementation.
// ARQ payload cache helpers + the C-function-pointer hooks the
// protocol layer (ALink) calls when ACKs arrive or RTOs expire.
//
// Trampolines (arqAckHookTrampoline, arqRetxHookTrampoline) are
// defined unconditionally so host tests can link against the
// AutoLink facade. The cache helpers (arqCache_*) are guarded by
// ARDUINO because they reference EspBlinkHal / malloc / free, which
// the host stubs don't model.
//
// The trampolines are no-ops on host (the cache helpers are guarded
// out, so calling them would crash) — host tests for AutoLink just
// verify the type compiles and constructs.
#include "AutoLink.h"

namespace autolink {

// C-linkage trampolines. ALink's ArqAckCallback / ArqRetxCallback
// are `bool (*)(uint8_t, void*)`; AutoLink's non-static member
// functions can't be used directly. These wrappers bridge.
//
// ack: called by the protocol with the BASE cobsSeq of the message
// that just had a chunk ACKed (the protocol translates chunkSeq ->
// baseSeq_[chunkSeq] before invoking us). We decrement the slot's
// chunks_left counter; the slot is freed only when it reaches 0.
// This handles multi-chunk messages: the header might be ACKed
// first while payload chunks are still in flight, and a retransmit
// of a payload chunk needs the cached payload to still be present.
//
// For keepalives (which are sent via sendCobsFrame_unlocked, not
// the ARQ path) and 1-chunk messages, the base seq equals the
// chunk seq and chunks_left starts at 1 — a single ACK frees the
// slot normally.
bool AutoLink::arqAckHookTrampoline(uint8_t ackedBaseSeq, void* ctx) {
    AutoLink* self = static_cast<AutoLink*>(ctx);
    int8_t idx = self->arqCache_findBySeq(ackedBaseSeq);
    if (idx < 0) {
        // No cache entry for this base — could be a keepalive ACK
        // (we don't cache keepalives) or a duplicate ACK. Drop.
        return false;
    }
    if (self->pending_[idx].chunks_left > 0) {
        self->pending_[idx].chunks_left--;
    }
    if (self->pending_[idx].chunks_left == 0) {
        self->arqCache_freeBySeq(ackedBaseSeq);
    }
    return false;
}

bool AutoLink::arqRetxHookTrampoline(uint8_t baseSeq, void* ctx) {
    AutoLink* self = static_cast<AutoLink*>(ctx);
    return self->arqCache_retx(baseSeq);
}

// v5 ARQ cache helpers. The facade caches one copy of every payload
// it sends, keyed by the cobsSeq ALink stamped on the wire. ACK-driven
// free (via arqAckHookTrampoline) reclaims the slot; retransmit (via
// arqRetxHookTrampoline) reads from the slot and re-sends through
// sendMsg. Memory: 32 slots * (cfg.maxMsg + ~16 bytes overhead) ≈ 32 KB
// at maxMsg=1024.
//
// v5.1.39 (one-owner design): the ARQ cache is keyed directly
// on the protocol's cobsSeq. The cache stores payload bytes for
// header entries; payload chunk entries are not populated. On a
// chunk ACK, the protocol translates chunk->base via baseSeq_[chunk]
// and calls arqCache_freeBySeq(base). All operations are under the
// protocol's link lock; the gate is checked via arqCache_hasRoom
// (also under the lock) before any stamping.

bool AutoLink::arqCache_hasRoom() {
    return pendingCount_ < ARQ_CACHE_CAP;
}

void AutoLink::arqCache_insert_unlocked(uint8_t baseSeq, const uint8_t* payload, int payloadLen, uint8_t chunkCount) {
    // v5.1.39 (one-owner design): the cache is keyed directly on
    // cobsSeq. If a previous entry exists at this seq, free it
    // first. (This happens if a retx's prior chunk ACKs cleaned
    // the protocol state but not the cache -- or if seq-space
    // wrapped. The gate (arqCache_hasRoom) ensures we don't
    // overflow overall, but doesn't prevent overwriting at a
    // specific seq.) The insert is called UNDER THE LINK LOCK
    // from sendMsgEx, so no concurrent writer can interleave.
    if (pending_[baseSeq].in_use) {
        free(pending_[baseSeq].buf);
        pending_[baseSeq].buf = nullptr;
        pending_[baseSeq].len = 0;
        pending_[baseSeq].chunks_left = 0;
        pending_[baseSeq].in_use = false;
        if (pendingCount_ > 0) pendingCount_--;
    }
    pending_[baseSeq].buf = (uint8_t*)malloc(payloadLen);
    if (!pending_[baseSeq].buf) {
        Log::log().error("AutoLink",
            "ARQ cache malloc failed for %d bytes (cobsSeq=%u)",
            payloadLen, (unsigned)baseSeq);
        return;
    }
    memcpy(pending_[baseSeq].buf, payload, payloadLen);
    pending_[baseSeq].len          = payloadLen;
    pending_[baseSeq].chunks_left  = chunkCount;  // 1 (header only) or 1+N (header + N payload chunks)
    pending_[baseSeq].chunks_total = chunkCount;  // v5.1.39: total at send time (for retx cleanup)
    pending_[baseSeq].in_use       = true;
    pendingCount_++;
}

int8_t AutoLink::arqCache_findBySeq(uint8_t seq) {
    if (seq >= ARQ_CACHE_SLOTS) return -1;
    if (!pending_[seq].in_use) return -1;
    return (int8_t)seq;
}

void AutoLink::arqCache_freeBySeq(uint8_t seq) {
    int8_t idx = arqCache_findBySeq(seq);
    if (idx < 0) return;
    if (pending_[idx].buf) {
        free(pending_[idx].buf);
        pending_[idx].buf = nullptr;
    }
    pending_[idx].in_use       = false;
    pending_[idx].len          = 0;
    pending_[idx].chunks_left  = 0;
    pending_[idx].chunks_total = 0;
    if (pendingCount_ > 0) pendingCount_--;
}

bool AutoLink::arqCache_retx(uint8_t seq) {
    int8_t idx = arqCache_findBySeq(seq);
    if (idx < 0) {
        // v5.1.39 (one-owner design): cache miss after a previous
        // retx already took the slot. The protocol still has
        // ackedPending_[s]=true for the original chunks. Clear
        // them via onAck so the timer stops firing for them. We
        // can't know chunks_total from the cleared slot, but
        // onAck is idempotent for non-pending seqs — it just
        // returns false. So calling onAck on seq..seq+7 (max
        // chunk count for cfg.maxMsg=1024) is safe: only the
        // actually-pending chunks clear; the rest are no-ops.
        Log::log().warning("AutoLink",
            "ARQ retransmit cache miss at cobsSeq=%u; clearing "
            "protocol state to break v5.1.38 cache-miss loop",
            (unsigned)seq);
        if (link) {
            ALink* lk = linkForTest();
            if (lk) {
                for (int i = 0; i < 8; i++) {
                    uint8_t chunkSeq = (uint8_t)(seq + i);
                    lk->onAck(chunkSeq);
                }
            }
        }
        return false;  // do not drop
    }
    // v5.1.39: use chunks_total (set at insert time, before any
    // ACKs decremented chunks_left). The cleanup needs to clear
    // ackedPending_ for ALL original chunks of the message, not
    // just the still-pending ones. Otherwise an already-ACKed
    // header chunk means chunks_left is one less than the total,
    // and the cleanup misses the actually-expired chunk.
    uint8_t origChunks = pending_[idx].chunks_total;
    Log::log().warning("AutoLink",
        "ARQ retransmit cobsSeq=%u (%d bytes, slot=%d)",
        (unsigned)seq, pending_[idx].len, (int)idx);
    // Free the OLD cache slot BEFORE retransmitting. The retransmit
    // goes through sendMsg() which allocates a NEW cobsSeq and
    // creates a NEW cache entry under it. (v5.1.14: bug #1 from the
    // user's code audit.) Split into two steps so the host test
    // can exercise the cache-management half without driving sendMsg:
    //   arqCache_takeRetxBuffer(seq, buf_out, len_out) frees the
    //     old slot and copies out the payload to resend;
    //   retx_resend(buf, len) calls sendMsg().
    uint8_t* buf = nullptr;
    int      len = 0;
    arqCache_takeRetxBuffer(seq, &buf, &len);
    retx_resend(buf, len);
    // v5.1.37 (closed-loop test): clean up the protocol's pending
    // state for the original chunks. The retx created new chunks
    // with new cobsSeqs, but the OLD chunks still have
    // ackedPending_[oldChunkSeq]=true. If we don't clear those,
    // the retransmit timer keeps firing for the original chunk
    // cobsSeqs, the cache misses (the cache entry is under the new
    // base seq now), the protocol drops the link 100ms after every
    // successful retx. We call link->onAck for each original
    // chunk seq.
    if (link && origChunks > 0) {
        ALink* lk = linkForTest();
        if (lk) {
            for (int i = 0; i < origChunks; i++) {
                uint8_t chunkSeq = (uint8_t)(seq + i);
                lk->onAck(chunkSeq);
            }
        }
    }
    return false;
}

// Take the payload out of the cache for retransmission and free the
// slot. Public on the facade (v5.1.14) so the host test can pin
// the fix without driving sendMsg.
void AutoLink::arqCache_takeRetxBuffer(uint8_t seq, uint8_t** bufOut, int* lenOut) {
    int8_t idx = arqCache_findBySeq(seq);
    if (idx < 0) {
        *bufOut = nullptr;
        *lenOut = 0;
        return;
    }
    *bufOut = pending_[idx].buf;
    *lenOut = pending_[idx].len;
    pending_[idx].buf          = nullptr;
    pending_[idx].len          = 0;
    pending_[idx].chunks_total = 0;
    pending_[idx].in_use       = false;
    if (pendingCount_ > 0) pendingCount_--;
}

// Re-send a previously-cached payload. (v5.1.37: fix for buffer
// leak — free the buffer after sendMsg returns.)
void AutoLink::retx_resend(const uint8_t* buf, int len) {
    if (buf == nullptr || len <= 0) return;
    sendMsg(buf, len);
    free((void*)buf);
}

// end of cache helpers

// v5.1.37: free every pending payload, zero pendingCount_, and
// reset seqToPending_ to "no mapping". Called by the link-reset
// trampoline from inside ALink::reset_unlocked, after the protocol
// has cleared its own ARQ maps and reset cobsSeq to 0. Without
// this, a link drop orphaned the cache: the old session's cobsSeqs
// were never reclaimed by arqCache_freeBySeq (the new session
// reuses low seqs first, doesn't sweep back through the old high
// range), pendingCount_ never returned to 0, and the v5.1.36
// cache-full gate latched on the very next sendMsg. Now the gate
// sees pendingCount_=0 and the link can recover.
//
// Called under the link lock (it's dispatched from reset_unlocked).
// Does NOT take any other lock — the facade's cache arrays are not
// protected by a lock of their own today, and the only writer on
// the host side is the user thread. On Arduino, sendMsg is called
// from the Arduino loopTask and reset_unlocked is also called from
// the loopTask (or from onTimer on the timer task, but the timer
// task is the one running onTimer — which has its own lock-free
// path through onPayload etc). In practice the cache is only
// written from the loop task and the lock-protected ALink paths,
// and the link lock serializes them.
// v5.1.39 (one-owner design): clear all cache entries. The
// cache is keyed on cobsSeq directly (pending_[256]), so we just
// walk the array. No seqToPending_[] to reset.
void AutoLink::arqCache_clearAll() {
    for (int i = 0; i < ARQ_CACHE_SLOTS; i++) {
        if (pending_[i].buf) {
            free(pending_[i].buf);
            pending_[i].buf = nullptr;
        }
        pending_[i].in_use       = false;
        pending_[i].len          = 0;
        pending_[i].chunks_left  = 0;
        pending_[i].chunks_total = 0;
    }
    pendingCount_ = 0;
    Log::log().info("AutoLink",
        "ARQ cache cleared (link reset): %d slot(s) freed", ARQ_CACHE_SLOTS);
}

// C-linkage trampoline that ALink::reset_unlocked calls to notify
// the facade that the link has been reset. ctx is the AutoLink*
// (set in the constructor via setLinkResetHook).
void AutoLink::linkResetHookTrampoline(void* ctx) {
    AutoLink* self = static_cast<AutoLink*>(ctx);
    if (self) self->arqCache_clearAll();
}

// v5.1.39: cache hook trampolines. Each takes the AutoLink* from
// ctx and dispatches to the corresponding cache method. The
// arqCache_insert_unlocked and arqCache_clearAll methods are
// already under the protocol's link lock when called from these
// trampolines (the protocol's own sendMsgEx / reset_unlocked
// hold the lock around the hook call). The hasRoom trampoline is
// also called under the lock by the protocol's sendMsgEx.

// hasRoom: gate check. Returns true if the cache has room for
// another message. The cache is keyed on cobsSeq (256 entries);
// the cap (240) leaves margin over the link's in-flight
// cobsSeqs.
bool AutoLink::arqCacheHasRoomTrampoline(void* ctx) {
    AutoLink* self = static_cast<AutoLink*>(ctx);
    if (!self) return false;
    return self->pendingCount_ < ARQ_CACHE_CAP;
}

// insert: store the payload under the protocol's stamped
// baseSeq. Called from sendMsgEx UNDER THE LINK LOCK, after all
// chunks have been stamped on the wire. The protocol has already
// incremented txSeq, so by the time we run here, the wire bytes
// carry the cobsSeq we're being asked to cache under. If the
// protocol drops the link between stamping and inserting (e.g.
// app buffer fills, onPayload returns true), the insert never
// runs -- the cache stays empty for that seq, which is correct:
// the chunk is treated as never-sent (no ACK will come, no
// retransmit needed, the new session after re-sweep uses a new
// seq space).
void AutoLink::arqCacheInsertTrampoline(uint8_t baseSeq, const uint8_t* payload, int payloadLen, uint8_t chunkCount, void* ctx) {
    AutoLink* self = static_cast<AutoLink*>(ctx);
    if (!self) return;
    // Direct indexing into pending_[baseSeq]. No seqToPending_[].
    // The cache is keyed on cobsSeq directly. If the slot is
    // already in_use, free it first (defensive: shouldn't happen).
    self->arqCache_insert_unlocked(baseSeq, payload, payloadLen, chunkCount);
}

// clearAll: free all cache entries. Called from reset_unlocked
// AFTER the protocol's own ARQ maps are cleared. Same signature
// as linkResetHookTrampoline; the v5.1.39 facade wires both to
// arqCache_clearAll.
void AutoLink::arqCacheClearAllTrampoline(void* ctx) {
    AutoLink* self = static_cast<AutoLink*>(ctx);
    if (self) self->arqCache_clearAll();
}

} // namespace autolink
