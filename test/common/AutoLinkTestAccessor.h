// Test-only shim for AutoLink. The test_*
// and *_ForTest methods (linkForTest,
// arqCacheForTest, arqCacheSizeForTest,
// test_arqCache_put, test_arqCache_hasRoom,
// test_arqPoolSize, test_arqCache_freeBySeq,
// test_arqCache_retx, test_arqCache_findBySeq,
// test_markAckedPending, test_sendMsgBeginForTest,
// test_sendMsgStillWaitingForTest,
// syncAckTimeoutMsForTest) are private on
// AutoLink; this shim is a `friend` and is
// the only path a host test reaches them through.
//
// Build only under -DAUTOLINK_HOST_TEST. Do
// NOT include from src/ or include/ headers.
#pragma once
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see test/test_desktop/Makefile)"
#endif

#include "AutoLink.h"
#include "LinkTestAccessor.h"
#include "al/link/arq/ArqCache.h"
#include <cstdint>

namespace autolink {

class AutoLinkTestAccessor {
public:
    explicit AutoLinkTestAccessor(AutoLink &a) : a_(a) {}
    explicit AutoLinkTestAccessor(const AutoLink &a)
        : a_(const_cast<AutoLink &>(a)) {}

    // Composed accessor: wraps the inner
    // link so reorder / sendMsg / etc. all
    // go through LinkTestAccessor too.
    LinkTestAccessor linkTest() { return LinkTestAccessor(*a_.link.get()); }

    // Direct facade accessors.
    int arqCacheSize() const { return a_.arqCache_.size(); }
    Link *link() { return a_.link.get(); }
    const Link *link() const { return a_.link.get(); }
    ArqCache *arqCache() { return &a_.arqCache_; }
    const ArqCache *arqCache() const { return &a_.arqCache_; }

    // ARQ cache fixtures used by
    // AutoLinkFacadeTest. The 4th
    // argument is a legacy chunkCount
    // ignored by the modern ArqCache.
    void arqCachePut(uint8_t seq, const uint8_t *b, int len, uint8_t) {
        a_.arqCache_.testPut(seq, b, len);
    }
    bool arqCacheHasRoom() { return a_.arqCache_.hasRoom(); }
    static int arqPoolSize() { return ArqCache::POOL_SIZE; }
    void arqCacheFreeBySeq(uint8_t s) { a_.arqCache_.freeBySeq(s); }
    bool arqCacheRetx(uint8_t seq) {
        const uint8_t *buf = nullptr;
        int len = 0;
        bool hit = a_.arqCache_.testRetx(seq, &buf, &len);
        (void)buf;
        (void)len;
        return hit;
    }
    int arqCacheFindBySeq(uint8_t s) {
        return a_.arqCache_.slotInUse(s) ? (int)s : -1;
    }

    // Pass-through to the link for
    // SYNC-mode tests and the ARQ
    // markAckedPending hook. Reaches
    // through the Link shim so we
    // never touch Link's privates
    // from here.
    void markAckedPending(uint8_t s) {
        if (a_.link)
            LinkTestAccessor(*a_.link).markAckedPending(s);
    }
    bool sendMsgBeginForTest(const uint8_t *b, int len) {
        return a_.link && LinkTestAccessor(*a_.link).sendMsgBegin(b, len);
    }
    bool sendMsgStillWaitingForTest() {
        return a_.link && LinkTestAccessor(*a_.link).sendMsgStillWaiting();
    }
    int syncAckTimeoutMsForTest() const {
        return a_.link ? LinkTestAccessor(*a_.link).syncAckTimeoutMs() : 500;
    }

    // Pass-through to the facade itself
    // for fixtures that already hold an
    // AutoLink&.
    AutoLink &facade() { return a_; }
    const AutoLink &facade() const { return a_; }

private:
    AutoLink &a_;
};

} // namespace autolink
