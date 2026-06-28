// User-facing config struct: baud list,
// timeouts, buffer sizes, mode. Owned by
// the facade / hal / link as a value;
// declared here so the hal layer can see
// it without pulling in the link layer.
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace autolink {

#ifndef AUTOLINK_MAX_BAUDS
#    define AUTOLINK_MAX_BAUDS 16
#endif

// ARQ pipeline window — the in-flight
// chunk budget the flow controller
// (Ping) drives the link toward. Owned
// semantically by Ping (it owns the
// outstanding-message ring buffer);
// declared here so AutoLink can pass the
// same value to ArqCache at construction
// time (the cache must hold a full
// window plus retx headroom — Ping.h
// re-exports it as `WINDOW`). Bumping
// this constant requires bumping
// ArqCache::POOL_SIZE to keep the
// POOL_SIZE >= 2*WINDOW guard honest;
// the cache validates at construction.
constexpr int AUTOLINK_ARQ_PIPELINE_WINDOW = 32;

// Pure-function helper. Given a message
// of `len` bytes, returns the number of
// chunked COBS frames the wire will carry
// (1 for short msgs that fit a hdr+payload
// coalesced frame, 1 + ceil(len/MAX_CHUNK)
// for long msgs: the hdr-only frame plus
// the per-chunk data frames). Used by
// Link::sendMsg() to guard against
// seq-space exhaustion (cobsSeq wraps at
// 254; aliasing live seqs silently
// corrupts the wire). Host-testable: no
// ARDUINO deps. MAX_CHUNK is declared in
// al/link/LinkContext.h, which the
// LinkApi TU pulls in; this header is
// included by host tests and the
// dashboard, neither of which sees
// LinkContext.h. Keep the formula in
// this header self-contained so callers
// don't have to chain include.
constexpr int chunksForMsgLen(int len) {
    if (len <= 0)
        return 0;
    // MAX_CHUNK = 250 + MSG_HDR = 6 is
    // the merged-coalesce cap (declared
    // in LinkContext.h / Link.h). Mirror
    // the exact constants here so the
    // helper is host-linkable without
    // pulling in Link.h.
    constexpr int kChunkCap = 250;
    constexpr int kHdr = 6;
    if (len + kHdr <= kChunkCap)
        return 1;
    int n = (len + kChunkCap - 1) / kChunkCap; // data chunks
    return 1 + n;                              // hdr frame + data
}

// Seq-space headroom for a single message:
// 254 (COBS_SEQ_MAX+1) minus the chunks
// this msg will consume. The link layer
// rejects a send when in-flight + new >
// COBS_SEQ_MAX so aliasing can't happen.
// 254 not 256: 0xFE/0xFF are reserved as
// NAK/ACK wire discriminators (see
// LinkFrameRx.h).
constexpr int COBS_SEQ_SPACE = 254;

// Compile-time wiring of the seq-space budget
// against the message-size budget. The runtime
// guard in Link::sendMsg() (inflight + chunks
// <= COBS_SEQ_SPACE) is only safe if a single
// message's chunk count fits the seq space
// alone — otherwise no number of in-flight
// messages can be zero and the guard trips on
// every send. Catches a future maxMsg bump
// (e.g. 32 KB) that would silently under-size
// the budget.
//
// arqChunkBudget is the steady-state chunk
// budget Ping drives the link toward: 2x
// window covers a full pipeline plus retx
// headroom. With window=32 and the cache's
// POOL_SIZE=64, the budget is 64. The chunks-
// for-one-msg formula gives 1 + ceil(maxMsg /
// 250); for maxMsg=1024 that's 1+5=6. So a
// 64-budget / 6-chunk message leaves 10
// messages in flight before the seq-space
// guard trips — comfortable margin.
//
// If a future bump raises maxMsg to 32 KB,
// 1 + ceil(32768/250) = 132 chunks, which is
// still under 254, but only just. A bump past
// ~62 KB (1+ceil(62000/250) = 249) trips this
// assert with a clear hint.
constexpr int ARQ_CHUNK_BUDGET = AUTOLINK_ARQ_PIPELINE_WINDOW * 2;
static_assert(ARQ_CHUNK_BUDGET >= 2 * AUTOLINK_ARQ_PIPELINE_WINDOW,
              "ARQ_CHUNK_BUDGET must cover a full window plus retx "
              "headroom; bump the cache POOL_SIZE");
static_assert(chunksForMsgLen(1024) <= COBS_SEQ_SPACE,
              "default maxMsg=1024 must fit the seq space alone; "
              "raise COBS_SEQ_SPACE (drop reserved discriminators) or "
              "lower MAX_CHUNK");
// The budget-vs-msg invariant: even if a future
// maxMsg bump raises chunks-per-msg, the
// steady-state budget must hold at least one
// full message worth of chunks. If this trips,
// either bump ARQ_CHUNK_BUDGET (requires
// ArqCache::POOL_SIZE bump) or lower maxMsg.
static_assert(ARQ_CHUNK_BUDGET >= chunksForMsgLen(1024) * 2,
              "ARQ_CHUNK_BUDGET must hold at least 2x max-chunks-per-msg "
              "so a single large message fits the seq space while at "
              "least one previous message is still in flight");

struct AutoLinkConfig {
    uint32_t allowedBauds[AUTOLINK_MAX_BAUDS] = { 115200, 57600, 38400, 19200,
                                                  9600 };
    // Allowed baud count is clamped at the
    // choke-point accessors in Link (see
    // Link::allowedBaudsCount() and
    // Link::allowedBaud(i)) — every read
    // goes through those, so a post-
    // construction write to this public
    // field with an out-of-range value
    // (e.g. 20) cannot drive spdI into an
    // OOB read of allowedBauds[]. The field
    // stays public so existing sketches can
    // keep setting it directly; the choke
    // points do the bounding.
    int allowedBaudsCount = 5;

    // In-place clamp. AutoLink calls this
    // on its incoming cfg before passing
    // it to Link so a caller that sets
    // allowedBaudsCount = 20 (or -1)
    // gets a sane value rather than an
    // OOB read through allowedBauds(i)
    // in the sweep / lockOk path.
    // Returns the clamped value.
    int clampToMaxBauds() {
        if (allowedBaudsCount < 0)
            allowedBaudsCount = 0;
        if (allowedBaudsCount > AUTOLINK_MAX_BAUDS)
            allowedBaudsCount = AUTOLINK_MAX_BAUDS;
        return allowedBaudsCount;
    }

    // Const clamp. Returns the clamped value
    // without mutating the field. The link
    // layer's per-call read paths use this
    // (cfg is stored by value in Link but the
    // field stays mutable, so a const this is
    // mostly useful when the cfg is `const
    // AutoLinkConfig &` at a call site — the
    // ctor of Link has that signature).
    int clampedCount() const {
        if (allowedBaudsCount < 0)
            return 0;
        if (allowedBaudsCount > AUTOLINK_MAX_BAUDS)
            return AUTOLINK_MAX_BAUDS;
        return allowedBaudsCount;
    }

    // Validating accessor. Returns the
    // element at `i` if `i` is in range,
    // else 0. Pairs with the ctor clamp
    // so the link layer's allowedBaud(i)
    // can fall back to a safe value even
    // if a caller bypasses clampToMaxBauds.
    uint32_t allowedBaudSafe(int i) const {
        if (i < 0 || i >= AUTOLINK_MAX_BAUDS)
            return 0;
        return allowedBauds[i];
    }

    int errThreshold = 100;
    int delayMs = 50;
    size_t rxBufferSize = 2048;
    size_t txBufferSize = 256;
    size_t streamBufferSize = 2048;
    size_t maxMsg = 1024;
    int ledPin = 2;
    int idleTimeoutMs = 10000;
    int pingSamplesPerBaud = 3;
    float minAcceptRate = 0.5f;
    int errRateWindow = 30;

    // Out-of-order frames held up to this
    // long; expired slots → lostMsgs++.
    int reorderHoldMs = 1500;

    // SYNC: stop-and-wait. One message in
    // flight at a time. Sender blocks for
    // the receiver ACK before sending the
    // next. No ARQ cache use, no reorder
    // buffer reserve, cobsSeq gaps dropped
    // instead of held. Default — works on
    // any wire that carries COBS+CRC
    // frames. ~half the throughput of
    // ASYNC at the cost of being boring
    // and reliable.
    // ASYNC: today's pipeline — ARQ cache,
    // reorder buffer, many in flight, async
    // retransmits on NAK / ACK-timeout.
    // Faster under good conditions, falls
    // apart under sustained noise. Both
    // boards must run the same mode.
    enum class Mode : uint8_t { SYNC = 0, ASYNC = 1 };
#ifdef AUTOLINK_HOST_TEST
    // Host tests have no FreeRTOS link task
    // to deliver ACKs and no wall clock;
    // SYNC's poll-with-yield wait would
    // spin. Default the host build to ASYNC
    // so existing tests don't hang. Arduino
    // sketches default to SYNC (boring and
    // reliable out of the box).
    Mode mode = Mode::ASYNC;
#else
    Mode mode = Mode::SYNC;
#endif

    // SYNC only: how long send() blocks
    // waiting for the receiver ACK before
    // timing out and returning 0. Also
    // used as the ASYNC ARQ retransmit
    // timeout (retransmit if no ACK
    // arrives within this window).
    int syncAckTimeoutMs = 500;

    // ARQ retransmit budget per chunk.
    // After this many unacknowledged
    // retransmits, the chunk is dropped
    // and the link is reset.
    uint8_t maxRetx = 5;

    // Per-transmit delay (ms) honored by
    // Ping::loop after each send. Lets
    // the GUI throttle the wire without
    // recompiling. 0 = no delay (run as
    // fast as the link allows). Works in
    // both SYNC and ASYNC; the SYNC
    // sender is already blocked on the
    // receiver ACK, so the additional
    // txDelayMs only adds idle time when
    // the wire is fast enough to clear
    // the ACK before the delay expires.
    //
    // this release: default raised 0 -> 50 ms.
    // The pre-this release default of 0 caused
    // Ping to flood the wire at full
    // link speed, which made ARQ-level
    // retransmits harder to diagnose and
    // starved the dashboard's /logs poll
    // for entries. The 50 ms default
    // gives the wire enough idle time
    // for /stats + /logs to keep up at
    // 5 bauds and makes gap-stop /
    // dropLink-on-N paths visible in
    // the log. The previous 100 ms was
    // too aggressive a throttle for the
    // higher baud table — operators
    // reported the dashboard dropping
    // below 10 msg/s, which masks
    // transport errors under too much
    // air-time. 50 ms hits a sweet spot
    // for the 5-baud sweep.
    int txDelayMs = 50;
};

} // namespace autolink