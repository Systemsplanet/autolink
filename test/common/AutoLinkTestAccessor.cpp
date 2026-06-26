// AutoLinkTestAccessor implementation.
// Friend access into AutoLink's private
// state. The shim is the only test
// surface that reaches the facade
// internals; production code does not
// include this TU.
#include "AutoLinkTestAccessor.h"

#include "LinkTestAccessor.h"

namespace autolink {

LinkTestAccessor AutoLinkTestAccessor::linkTest() {
    return LinkTestAccessor(*a_.link);
}

int AutoLinkTestAccessor::arqCacheSize() const { return a_.arqCache_.size(); }
Link *AutoLinkTestAccessor::link() { return a_.link.get(); }
const Link *AutoLinkTestAccessor::link() const { return a_.link.get(); }
ArqCache *AutoLinkTestAccessor::arqCache() { return &a_.arqCache_; }
const ArqCache *AutoLinkTestAccessor::arqCache() const { return &a_.arqCache_; }

void AutoLinkTestAccessor::arqCachePut(uint8_t seq, const uint8_t *b, int len,
                                       uint8_t) {
    a_.arqCache_.testPut(seq, b, len);
}

bool AutoLinkTestAccessor::arqCacheHasRoom() { return a_.arqCache_.hasRoom(); }

int AutoLinkTestAccessor::arqPoolSize() { return ArqCache::POOL_SIZE; }

void AutoLinkTestAccessor::arqCacheFreeBySeq(uint8_t s) {
    a_.arqCache_.freeBySeq(s);
}

bool AutoLinkTestAccessor::arqCacheRetx(uint8_t seq) {
    const uint8_t *buf = nullptr;
    int len = 0;
    bool hit = a_.arqCache_.testRetx(seq, &buf, &len);
    (void)buf;
    (void)len;
    return hit;
}

int AutoLinkTestAccessor::arqCacheFindBySeq(uint8_t s) {
    return a_.arqCache_.slotInUse(s) ? (int)s : -1;
}

void AutoLinkTestAccessor::markAckedPending(uint8_t s) {
    if (a_.link)
        LinkTestAccessor(*a_.link).markAckedPending(s);
}

bool AutoLinkTestAccessor::sendMsgBeginForTest(const uint8_t *b, int len) {
    return a_.link && LinkTestAccessor(*a_.link).sendMsgBegin(b, len);
}

bool AutoLinkTestAccessor::sendMsgStillWaitingForTest() {
    return a_.link && LinkTestAccessor(*a_.link).sendMsgStillWaiting();
}

int AutoLinkTestAccessor::syncAckTimeoutMsForTest() const {
    return a_.link ? LinkTestAccessor(*a_.link).syncAckTimeoutMs() : 500;
}

} // namespace autolink