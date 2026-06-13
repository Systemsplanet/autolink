#pragma once
#include <stdint.h>

// ----------------------------------------------------------------------------
// UtilFrameRx — reliable-mode receive accumulator.
//
// Feed it raw wire bytes; it collects 0x00-delimited COBS frames, decodes
// them, verifies the trailing CRC-8, and hands validated payloads to a
// Listener. Bad CRC, malformed COBS, CRC-only frames, and oversize frames
// are reported as frame errors.
//
// The keepalive is the two-byte sequence 0x00 0x00. feed() consumes the
// pair at a clean boundary with no callback; mid-COBS the first 0x00 closes
// the partial (one onFrameError) and the second is skipped. Stray single
// 0x00s are still skipped for back-compat.
//
// If the Listener reports a link drop, feed() stops and returns how far it
// got so the caller can hand the rest of the event to its command parser.
// No locking, no allocation; thread safety is the caller's job.
// ----------------------------------------------------------------------------

namespace autolink {

class UtilFrameRx {
public:
    // Listener — implemented by the owner (ALink) to receive validated frame
    // payloads and frame-error notifications. Both callbacks return true if
    // the link was dropped mid-event so feed() can stop early and hand the
    // rest of the buffer to the command parser.
    class Listener {
    public:
        virtual ~Listener() {}
        virtual bool onPayload(const uint8_t* b, int n) = 0; // validated payload
        virtual bool onFrameError() = 0;                     // corrupt/oversize frame
    };

    explicit UtilFrameRx(Listener& l) : lis(l) {}

    // Consume up to len bytes; returns bytes consumed (== len unless a
    // callback reported a link drop).
    int feed(const uint8_t* data, int len);

    // Discard any partial frame (call on link drop / re-sweep).
    void reset();

private:
    Listener& lis;
    uint8_t buf[256];      // raw COBS bytes of the frame being collected
    int     idx = 0;
    uint8_t decoded[256];  // scratch for the decoded payload + CRC
};

} // namespace autolink
