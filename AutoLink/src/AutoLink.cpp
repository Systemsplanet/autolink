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
// Compiled on host too (no ARDUINO gate): malloc/free/memcpy are
// available in the host test build, so the tests can directly
// exercise arqCache_put/retx and pin the bug fix from v5.1.14.
void AutoLink::arqCache_put(uint8_t seq, const uint8_t* b, int len, uint8_t chunkCount) {
    // Free any existing payload under this seq (shouldn't happen
    // unless the cache map is corrupt; cheap to be safe).
    arqCache_freeBySeq(seq);
    int slot = -1;
    for (int i = 0; i < ARQ_CACHE_SLOTS; i++) {
        if (!pending_[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        Log::log().error("AutoLink",
            "ARQ cache full (%d pending); dropping cache entry for cobsSeq=%u",
            ARQ_CACHE_SLOTS, (unsigned)seq);
        return;
    }
    pending_[slot].buf = (uint8_t*)malloc(len);
    if (!pending_[slot].buf) {
        Log::log().error("AutoLink",
            "ARQ cache malloc failed for %d bytes", len);
        return;
    }
    memcpy(pending_[slot].buf, b, len);
    pending_[slot].len         = len;
    pending_[slot].seq         = seq;
    pending_[slot].chunks_left = chunkCount;  // 1 (header only) or 1+N (header + N payload chunks)
    pending_[slot].in_use      = true;
    seqToPending_[seq]         = (int8_t)slot;
    pendingCount_++;
}

int8_t AutoLink::arqCache_findBySeq(uint8_t seq) {
    int8_t idx = seqToPending_[seq];
    if (idx < 0 || idx >= ARQ_CACHE_SLOTS) return -1;
    if (!pending_[idx].in_use || pending_[idx].seq != seq) return -1;
    return idx;
}

void AutoLink::arqCache_freeBySeq(uint8_t seq) {
    int8_t idx = arqCache_findBySeq(seq);
    if (idx < 0) return;
    if (pending_[idx].buf) {
        free(pending_[idx].buf);
        pending_[idx].buf = nullptr;
    }
    pending_[idx].in_use      = false;
    pending_[idx].seq         = 0;
    pending_[idx].len         = 0;
    pending_[idx].chunks_left = 0;
    seqToPending_[seq]        = -1;
    if (pendingCount_ > 0) pendingCount_--;
}

bool AutoLink::arqCache_retx(uint8_t seq) {
    int8_t idx = arqCache_findBySeq(seq);
    if (idx < 0) {
        // Cache miss: seq was never sent by us, or cache already freed
        // (stale retransmit trigger). Protocol will drop on MAX_RETX.
        Log::log().error("AutoLink",
            "ARQ retransmit requested for cobsSeq=%u but no cache slot; "
            "the link will drop", (unsigned)seq);
        return true;  // drop
    }
    // v5.1.37 (closed-loop test): capture the original base seq +
    // chunk count BEFORE takeRetxBuffer clears the slot. We need
    // these to clean up the protocol's pending state for the
    // ORIGINAL chunks after the retx succeeds. Without this cleanup,
    // the protocol's retransmit timer keeps firing for the original
    // chunk cobsSeqs (which are now stale — the new chunks use
    // different cobsSeqs), the cache miss fires again, the link
    // drops within 100ms of every successful retx on a noisy wire.
    // This is the cache-miss loop the WireSim closed-loop test
    // surfaces: pre-v5.1.37, every retx on a lossy link was
    // followed by a forced link drop.
    uint8_t origBase = pending_[idx].seq;
    uint8_t origChunks = pending_[idx].chunks_left;
    Log::log().warning("AutoLink",
        "ARQ retransmit cobsSeq=%u (%d bytes, slot=%d)",
        (unsigned)seq, pending_[idx].len, (int)idx);
    // Free the OLD cache slot BEFORE retransmitting. The retransmit
    // goes through sendMsg() which allocates a NEW cobsSeq and
    // creates a NEW cache entry under it. If we didn't free the old
    // slot here, every retx would leak a slot; after MAX_RETX
    // retransmits the 32-slot cache would fill up and the link
    // would drop with "ARQ cache full". (v5.1.14: bug #1 from the
    // user's code audit.) The retransmit is its own message from the
    // cache's perspective: the peer sees a new cobsSeq and ACKs it,
    // freeing the new slot. The old slot, with its now-stale
    // cobsSeq, is gone before the new entry is created.
    //
    // Split into two steps so the host test can exercise the
    // cache-management half without driving sendMsg (which needs
    // a live stream buffer the host doesn't have):
    //   arqCache_takeRetxBuffer(seq, buf_out, len_out) frees the
    //     old slot and copies out the payload to resend;
    //   retx_resend(buf, len) calls sendMsg().
    uint8_t* buf = nullptr;
    int      len = 0;
    arqCache_takeRetxBuffer(seq, &buf, &len);
    retx_resend(buf, len);
    // v5.1.37 (closed-loop test): clean up the protocol's pending
    // state for the original chunks. The retx created new chunks
    // with new cobsSeqs (via sendMsg -> sendCobsFrameAcked_unlocked
    // inside retx_resend), but the OLD chunks still have
    // ackedPending_[oldChunkSeq]=true. If we don't clear those, the
    // retransmit timer keeps firing every 100ms for each old
    // chunk, the cache misses (the cache entry is under the new
    // base seq now), the protocol drops the link 100ms after every
    // successful retx. On a noisy wire this means the link drops
    // every 100ms — completely broken.
    //
    // We call link->onAck for the base seq and each chunk seq of
    // the original message. onAck clears ackedPending_[s]=false
    // and calls back into the facade (no-op because the cache
    // entry was already taken above). The link is left in a clean
    // state where only the NEW chunks' pending state exists.
    if (link && origChunks > 0) {
        // v5.1.37: capture the link pointer from the trampoline
        // context. The protocol layer doesn't expose a direct
        // getter; we use linkForTest() which is the same pointer
        // the protocol layer holds.
        ALink* lk = linkForTest();
        if (lk) {
            for (int i = 0; i < origChunks; i++) {
                uint8_t chunkSeq = (uint8_t)(origBase + i);
                lk->onAck(chunkSeq);
            }
        }
    }
    return false;
}

// Take the payload out of the cache for retransmission and free the
// old slot. Public on the facade (v5.1.14) so the host test can pin
// the fix without driving sendMsg.
void AutoLink::arqCache_takeRetxBuffer(uint8_t seq, uint8_t** bufOut, int* lenOut) {
    int8_t idx = arqCache_findBySeq(seq);
    if (idx < 0) {
        // Cache miss: nothing to take.
        *bufOut = nullptr;
        *lenOut = 0;
        return;
    }
    *bufOut = pending_[idx].buf;
    *lenOut = pending_[idx].len;
    // Clear the slot's buf pointer so freeBySeq doesn't double-free.
    pending_[idx].buf = nullptr;
    pending_[idx].len = 0;
    pending_[idx].in_use = false;
    seqToPending_[seq]  = -1;
    if (pendingCount_ > 0) pendingCount_--;
}

// Re-send a previously-cached payload. Called by arqCache_retx after
// the old slot has been freed. Public on the facade (v5.1.14) so the
// test can call arqCache_takeRetxBuffer + retx_resend as two phases.
//
// v5.1.37: this previously leaked the `buf` pointer (taken from the
// cache by arqCache_takeRetxBuffer, ownership transferred to us).
// sendMsg memcpys the payload into a NEW cache slot keyed under a
// fresh cobsSeq, then returns — leaving the old buffer still
// malloc'd. Every retransmit leaked `len` bytes; after MAX_RETX
// retransmits with full-size messages (1024 B), up to 160 KB of
// heap gone. On ESP32, eventual OOM. Fix: free the buffer after
// sendMsg returns, regardless of whether sendMsg succeeded
// (sendMsg copies the bytes synchronously under the lock, so
// ownership of the source bytes transfers to us on return).
void AutoLink::retx_resend(const uint8_t* buf, int len) {
    if (buf == nullptr || len <= 0) return;
    sendMsg(buf, len);
    // Ownership of `buf` was transferred to us by
    // arqCache_takeRetxBuffer. The bytes have been memcpyd into
    // the new cache slot by sendMsg, so the source buffer is
    // unreachable from any data structure. Free it.
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
void AutoLink::arqCache_clearAll() {
    for (int i = 0; i < ARQ_CACHE_SLOTS; i++) {
        if (pending_[i].buf) {
            free(pending_[i].buf);
            pending_[i].buf = nullptr;
        }
        pending_[i].in_use      = false;
        pending_[i].seq         = 0;
        pending_[i].len         = 0;
        pending_[i].chunks_left = 0;
    }
    memset(seqToPending_, -1, sizeof(seqToPending_));
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

} // namespace autolink
