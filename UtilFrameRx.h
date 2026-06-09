#pragma once
#include <stdint.h>

// ----------------------------------------------------------------------------
// UtilFrameRx — reliable-mode receive accumulator.
//
// Feed it raw wire bytes; it collects 0x00-delimited COBS frames, decodes
// them, verifies the trailing CRC-8, and hands validated payloads to a
// Listener. Bad CRC, malformed COBS, CRC-only frames, and oversize frames
// are reported as frame errors. Stray zeros between frames (the link
// keepalive) are skipped silently. If the Listener reports that the link
// dropped, feed() stops consuming and returns early so the caller can hand
// the rest of the event to its command parser. No locking, no allocation;
// thread safety is the caller's job.
// ----------------------------------------------------------------------------

namespace autolink {

class UtilFrameRx {
public:
    // Receiver-side callbacks. Both return true if the link was dropped and
    // feeding this event should stop.
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
