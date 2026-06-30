// User-facing config struct: baud list,
// timeouts, buffer sizes, mode. Owned by
// the facade / hal / link as a value;
// declared here so the hal layer can see
// it without pulling in the link layer.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "al/link/LinkContext.h"

namespace autolink {

#ifndef AUTOLINK_MAX_BAUDS
#    define AUTOLINK_MAX_BAUDS 16
#endif

struct AutoLinkConfig;

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
// ARDUINO deps. MAX_CHUNK and MSG_HDR
// are declared in al/link/LinkContext.h
// (included at file top). The floor
// helpers below also reference
// MAX_CHUNK symbolically — one source
// of truth so a chunk-cap bump in
// LinkContext.h can't desync the
// seq-space math from the wire cap.
constexpr int chunksForMsgLen(int len) {
    if (len <= 0)
        return 0;
    // MAX_CHUNK is the merged-coalesce
    // cap (hdr + payload fit in one
    // frame). MSG_HDR is the per-message
    // header length carried in the
    // first (hdr-only) chunk frame for
    // long messages.
    if (len + MSG_HDR <= MAX_CHUNK)
        return 1;
    int n = (len + MAX_CHUNK - 1) / MAX_CHUNK; // data chunks
    return 1 + n;                              // hdr frame + data
}

// Floor for the EspHal RX stream buffer.
// One full coalesced message plus a
// retransmit's worth of headroom (2×
// maxMsg + hdr padding). The RX staging
// buffer is not the ARQ cache — that
// lives in ArqCache::POOL_BUF_MAX. A
// FreeRTOS stream buffer can't be
// resized in flight, so the floor has
// to cover both SYNC (one in flight)
// and ASYNC (many in flight) without
// paying for a 16-slot pipeline. A
// caller-set cfg.streamBufferSize
// larger than the floor wins.
//
// Host-linkable: the formula is pure
// arithmetic. Host tests pin the
// heap-realistic ceiling at the default
// maxMsg. Declared here (forward of the
// AutoLinkConfig definition), defined
// after the struct so the inline body
// can see the field layout.
//
// Floor helpers reference MAX_CHUNK
// symbolically (al/link/LinkContext.h
// is included at file top) so a
// MAX_CHUNK bump can't desync the
// floor math from the wire-protocol
// chunk cap the framer honors. The
// chunk/frame-overhead pair here must
// match what the framer emits on the
// wire — see LinkTx.cpp for the
// MAX_CHUNK + MSG_HDR coalesced-frame
// cap and the UtilCobs encap overhead.
inline size_t streamBufferFloor(const AutoLinkConfig &cfg);

// Default user-facing maxMsg. Pulled
// out so the seq-space static_asserts,
// PingPongBase::BUF_SIZE, and the
// AutoLinkConfig field default all
// agree on one number. The runtime
// guard in Link::sendMsg() still
// rejects any single message that
// would consume too many chunks
// regardless of the default; this
// constant only owns the default
// shape.
constexpr size_t AUTOLINK_DEFAULT_MAX_MSG = 5120;

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
// 250); for maxMsg=5120 that's 1+21=22. So a
// 64-budget / 22-chunk message leaves ~2
// messages in flight before the seq-space
// guard trips — comfortable margin for the
// new default.
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
static_assert(chunksForMsgLen((int)AUTOLINK_DEFAULT_MAX_MSG) <= COBS_SEQ_SPACE,
              "default maxMsg must fit the seq space alone; "
              "raise COBS_SEQ_SPACE (drop reserved discriminators) or "
              "lower MAX_CHUNK");
// The budget-vs-msg invariant: even if a future
// maxMsg bump raises chunks-per-msg, the
// steady-state budget must hold at least one
// full message worth of chunks. If this trips,
// either bump ARQ_CHUNK_BUDGET (requires
// ArqCache::POOL_SIZE bump) or lower maxMsg.
static_assert(ARQ_CHUNK_BUDGET >=
                  chunksForMsgLen((int)AUTOLINK_DEFAULT_MAX_MSG) * 2,
              "ARQ_CHUNK_BUDGET must hold at least 2x max-chunks-per-msg "
              "so a single large message fits the seq space while at "
              "least one previous message is still in flight");

struct AutoLinkConfig {
    uint32_t allowedBauds[AUTOLINK_MAX_BAUDS] = { 512000, 115200, 57600,
                                                  38400,  19200,  9600 };
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
    int allowedBaudsCount = 6;

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
    size_t maxMsg = AUTOLINK_DEFAULT_MAX_MSG;
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
    // this release: default lowered 50 -> 0 ms.
    // The 6.0.1 default of 50 ms gave the
    // /stats + /logs poll enough headroom
    // when the dashboard was the bottleneck,
    // but a fresh sketch that boots without
    // WiFi (and therefore without the
    // dashboard's poll pressure) ships at
    // ~half the wire's natural throughput
    // for no operator-visible reason. 0 ms
    // restores full line-rate by default;
    // the dashboard dropdown still defaults
    // to 0 and operators who want a throttled
    // bench run can pick a value via the
    // widget or set cfg.txDelayMs directly.
    // The 50 ms default was originally
    // raised from 0 to 100 to 50 across
    // 5.3.x / 6.0.1 — the rationale is
    // still valid when the dashboard is
    // up, but the new default trusts the
    // caller to add throttling when they
    // need it.
    int txDelayMs = 0;
};

// Defined after the struct so the inline body
// can see the field layout. Body is pure
// arithmetic; see the forward-decl comment
// above for the rationale.
inline size_t streamBufferFloor(const AutoLinkConfig &cfg) {
    constexpr int kHdr = 6;
    constexpr int multiples = 2;
    size_t floor = (size_t)multiples * (cfg.maxMsg + kHdr);
    return cfg.streamBufferSize > floor ? cfg.streamBufferSize : floor;
}

// UART driver buffer sizing for the link layer.
// SYNC ships one chunk at a time and the
// receiver's window is bounded by a single
// coalesced frame — the 2048 rx / 256 tx
// defaults are enough. ASYNC pipelines many
// chunks in flight; the rx buffer must hold
// at least a full in-flight window plus
// headroom or the UART overruns and seq
// numbers go missing. The 32-slot
// AUTOLINK_ARQ_PIPELINE_WINDOW * 1.25
// headroom scales with the same window the
// ARQ cache uses (POOL_SIZE = 2 * window),
// so the rx buffer is consistent with the
// ARQ budget. Caller-set cfg.rxBufferSize /
// cfg.txBufferSize still win if they're
// larger than the floor.
//
// Host-linkable pure arithmetic. EspHal
// calls this in begin() (and in setMode
// pre-begin()) so the buffers track the
// restored mode.
inline size_t uartRxBufferFloor(const AutoLinkConfig &cfg) {
    // kFrameOverhead is the COBS encap +
    // zero-frame-delimiter pair the
    // framer emits on the wire around
    // every chunk. It does NOT include
    // MSG_HDR — that's a header inside
    // the chunk payload, already counted
    // by MAX_CHUNK (which bounds the
    // payload length, not the on-wire
    // byte count). Derive perChunk from
    // MAX_CHUNK so a future chunk-cap
    // bump can't desync the floor from
    // the framer.
    constexpr int kFrameOverhead = 4; // 0x00 + cobs overhead + 0x00
    constexpr int kHeadroomMul = 5;   // 1.25x pipeline window
    constexpr int kHeadroomDiv = 4;
    if (cfg.mode == AutoLinkConfig::Mode::SYNC)
        return cfg.rxBufferSize; // 2048 already plenty
    size_t perChunk = (size_t)MAX_CHUNK + kFrameOverhead;
    size_t window = (size_t)AUTOLINK_ARQ_PIPELINE_WINDOW;
    size_t floor = (perChunk * window * kHeadroomMul) / kHeadroomDiv;
    return cfg.rxBufferSize > floor ? cfg.rxBufferSize : floor;
}

inline size_t uartTxBufferFloor(const AutoLinkConfig &cfg) {
    constexpr int kFrameOverhead = 4;
    constexpr int kHeadroomMul = 3; // 1.5x of one in-flight chunk
    constexpr int kHeadroomDiv = 2;
    if (cfg.mode == AutoLinkConfig::Mode::SYNC)
        return cfg.txBufferSize; // 256 already plenty
    size_t perChunk = (size_t)MAX_CHUNK + kFrameOverhead;
    size_t floor = (perChunk * kHeadroomMul) / kHeadroomDiv;
    return cfg.txBufferSize > floor ? cfg.txBufferSize : floor;
}

} // namespace autolink