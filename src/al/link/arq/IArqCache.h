
#pragma once

#include <cstdint>

namespace autolink {
class IArqCache {
public:
    virtual ~IArqCache() = default;

    virtual bool hasRoom() const = 0;

    // min(free slots, free pool buffers) — how many
    // insertions fit right now.
    virtual int freeRoom() const = 0;

    virtual void insert(uint8_t seq, const uint8_t *payload,
                        int payloadLen) = 0;

    virtual void freeBySeq(uint8_t seq) = 0;

    virtual bool peekForRetx(uint8_t seq, const uint8_t **outBuf,
                             int *outLen) const = 0;

    virtual void clearAll() = 0;

    virtual bool slotInUse(uint8_t seq) const = 0;

    virtual int size() const = 0;

    // GBN pipeline depth: sendMsg's admission gate caps
    // inflight+chunks at this value.
    virtual int window() const = 0;
};
} // namespace autolink
