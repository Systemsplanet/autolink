// Pure interface for the ARQ payload
// cache. Split out of the AutoLink
// facade and behind an interface so
// the link layer can drive timing
// and the cache can stay pure
// storage. Mirrors the IHal pattern
// (AGENTS #10: virtual only at the
// user-extension boundary).
#pragma once

#include <cstdint>

namespace autolink {
class IArqCache {
public:
    virtual ~IArqCache() = default;

    // True iff at least one slot AND
    // one pool buffer are free. Pool-
    // bound, not slot-bound.
    virtual bool hasRoom() const = 0;

    // Insert (or replace) a payload for
    // cobsSeq. payload == nullptr or
    // payloadLen <= 0 records a zero-
    // length slot (keepalive). payloadLen
    // > POOL_BUF_MAX logs and skips.
    // Pool exhaustion logs and skips.
    virtual void insert(uint8_t seq, const uint8_t *payload,
                        int payloadLen) = 0;

    // Free a slot on ACK. No-op if
    // seq absent or out of range.
    virtual void freeBySeq(uint8_t seq) = 0;

    // Read-only peek for the retx path.
    // Returns true and writes the buffer
    // pointer + length if the seq has a
    // pool buffer. Returns false if the
    // seq is missing, in_use but no
    // buffer (keepalive), or poolIdx
    // out of range. Borrowed; the caller
    // must not retain across a freeBySeq
    // or clearAll.
    virtual bool peekForRetx(uint8_t seq, const uint8_t **outBuf,
                             int *outLen) const = 0;

    // Drop everything. Called on link
    // reset / re-sweep.
    virtual void clearAll() = 0;

    // Existence test — does seq have a
    // pending slot at all? Used by the
    // retx path to distinguish "cache
    // miss" from "keepalive slot".
    virtual bool slotInUse(uint8_t seq) const = 0;

    // Number of in-use slots. For test
    // + invariant use.
    virtual int size() const = 0;
};
} // namespace autolink
