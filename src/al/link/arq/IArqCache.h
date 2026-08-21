
#pragma once

#include <cstdint>

namespace autolink {
class IArqCache {
public:
    // Why a slot is being freed. See ArqCache.h for
    // the full rationale; declared here so the
    // IArqCache virtual can take it as a default
    // arg.
    enum class FreeCause : uint8_t {
        SingleAck = 0,
        CumulativeBackfill = 1,
        NakCumulative = 2,
        HonestDrop = 3,
        Reset = 4,
    };

public:
    virtual ~IArqCache() = default;

    virtual bool hasRoom() const = 0;

    // min(free slots, free pool buffers) — how many
    // insertions fit right now.
    virtual int freeRoom() const = 0;

    virtual void insert(uint8_t seq, const uint8_t *payload,
                        int payloadLen) = 0;

    virtual void freeBySeq(uint8_t seq) = 0;
    // Cause-tagged free. The default forwards to
    // freeBySeq(seq); ArqCache overrides both. The
    // virtual keeps the wire log's cause tag
    // accessible from callers that hold an
    // IArqCache& (Link is the only such caller, but
    // the indirection is the public contract).
    // Pinned by ArqCacheFreeCauseTest.
    virtual void freeBySeq(uint8_t seq, FreeCause cause) {
        (void)cause;
        freeBySeq(seq);
    }

    virtual bool peekForRetx(uint8_t seq, const uint8_t **outBuf,
                             int *outLen) const = 0;

    virtual void clearAll() = 0;

    virtual bool slotInUse(uint8_t seq) const = 0;

    virtual int size() const = 0;

    // GBN pipeline depth: sendMsg's admission gate caps
    // inflight+chunks at this value.
    virtual int window() const = 0;

    // Runtime shrink of the admission cap, never above the
    // window the cache was constructed with. Default no-op:
    // fixed-window callers (tests, NullArqCache) don't need to
    // participate. ArqCache overrides it so Link::begin() can
    // clamp admission to what the HAL's actual TX ring can hold
    // once that's known (a compile-time window can outrun a
    // heap-clamped ring). Pinned by
    // PipelineWindowClampedToTxRingTest.
    virtual void setWindow(int w) { (void)w; }
};
} // namespace autolink
