// Narrow I/O surface the link helpers
// (LinkArq, LinkReorder, LinkSweep) see.
// Pure abstract — Link implements every
// method. Friendships with the helpers
// were removed in favour of this
// interface, so the helpers can drive
// the link without reaching into
// Link's privates.
//
// One vtable per Link, on par with the
// existing IHal cost. No std::function
// or virtual bases on the helper side —
// helpers hold LinkContext& by
// reference and call through it.
//
// The CTRL command codes and MAX_CHUNK
// are wire-protocol values that the
// link and its helpers must agree on,
// so they live here alongside the
// surface the helpers see.
#pragma once
#include <stdint.h>

namespace autolink {

// CTRL-frame command codes. Data
// payloads use the cobsSeq space and
// are not driven through LinkContext.
constexpr uint8_t PING_CMD = 0x22;
constexpr uint8_t PONG_CMD = 0x33;
constexpr uint8_t REQ_CMD = 0x11;
constexpr uint8_t LOCK_CMD = 0x44;

// Per-frame payload cap. Reorder pool
// and link frame builder must agree.
constexpr int MAX_CHUNK = 250;

class LinkContext {
public:
    virtual ~LinkContext() = default;

    // Caller holds the link lock — these
    // are the unlocked halves of the I/O
    // path. They run under hw.lock() in
    // production; the contract here is
    // "caller owns the lock", not "these
    // take it themselves".
    virtual void hwLock() = 0;
    virtual void hwUnlock() = 0;
    virtual uint32_t hwNowMs() const = 0;
    virtual void hwSetSpd(uint32_t b) = 0;
    virtual void hwStartTimer(int ms) = 0;

    // CTRL-frame wire emit used by the
    // sweep phase machine. LinkSweep
    // drives PING/PONG/LOCK/REQ through
    // this one entry point; the body's
    // specifics (CRC, COBS) stay in
    // Link.cpp.
    virtual void sendFrame(uint8_t payload) = 0;

    // Master/slave role read by the sweep
    // phase machine to pick master vs
    // slave dwell shapes.
    virtual bool masterRole() const = 0;

    // Sweep phase machine tracks the
    // current baud index on Link — the
    // allowed-bauds table and the active
    // index move together as phases
    // transition.
    virtual int currentSpdI() const = 0;
    virtual void setCurrentSpdI(int i) = 0;
    virtual int allowedBaudsCount() const = 0;
    virtual uint32_t allowedBaud(int i) const = 0;
    virtual int delayMs() const = 0;

    // Reorder buffer callbacks. LinkReorder
    // hands the held payload to the link's
    // app-buffer queue, asks the link to
    // emit the matching ACK, advances the
    // rx-seq cursor, and tallies bytes for
    // stats — all under the same lock the
    // link holds across a flush.
    virtual int reorderPushAppBuf(const uint8_t *b, int n) = 0;
    virtual void reorderSendAck(uint8_t seq) = 0;
    virtual uint8_t reorderExpectedSeq() const = 0;
    virtual void reorderAdvanceRxSeq(uint8_t seq) = 0;
    virtual void reorderCountBytes(int n) = 0;
};

} // namespace autolink