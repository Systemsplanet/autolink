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
        if (allowedBaudsCount < 0) return 0;
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
    // this release: default raised 0 -> 100 ms.
    // The pre-this release default of 0 caused
    // Ping to flood the wire at full
    // link speed, which made ARQ-level
    // retransmits harder to diagnose and
    // starved the dashboard's /logs poll
    // for entries. The 100 ms default
    // gives the wire enough idle time
    // for /stats + /logs to keep up at
    // 5 bauds and makes gap-stop /
    // dropLink-on-N paths visible in
    // the log.
    int txDelayMs = 100;
};

} // namespace autolink