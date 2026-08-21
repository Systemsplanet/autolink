// Friend shim for the AutoLink facade. Build under
// -DAUTOLINK_HOST_TEST only; never include from src/ or include/.
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

    int arqCacheSize() const { return a_.arqCache_.size(); }
    Link *link() { return a_.link.get(); }
    const Link *link() const { return a_.link.get(); }
    ArqCache *arqCache() { return &a_.arqCache_; }
    const ArqCache *arqCache() const { return &a_.arqCache_; }

    // 4th arg is a legacy chunkCount the modern ArqCache ignores.
    void arqCachePut(uint8_t seq, const uint8_t *b, int len, uint8_t) {
        a_.arqCache_.testPut(seq, b, len);
    }
    bool arqCacheHasRoom() { return a_.arqCache_.hasRoom(); }
    static int arqPoolSize() { return ArqCache::POOL_SIZE; }
    void arqCacheFreeBySeq(uint8_t s) { a_.arqCache_.freeBySeq(s); }
    bool arqCacheRetx(uint8_t seq) {
        const uint8_t *buf = nullptr;
        int len = 0;
        return a_.arqCache_.testRetx(seq, &buf, &len);
    }
    int arqCacheFindBySeq(uint8_t s) {
        return a_.arqCache_.slotInUse(s) ? (int)s : -1;
    }

    // Routed through the Link shim so the facade never touches Link's
    // privates itself.
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

private:
    AutoLink &a_;
};

} // namespace autolink
