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

struct AutoLinkConfig {
    uint32_t allowedBauds[AUTOLINK_MAX_BAUDS] = { 115200, 57600, 38400, 19200,
                                                  9600 };
    int allowedBaudsCount = 5;
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
    int txDelayMs = 0;

    bool _test_forwardResync = false;
};

} // namespace autolink