#pragma once
#include <stdint.h>

// ----------------------------------------------------------------------------
// UtilFrameRx — reliable-mode receive accumulator (v4.0.0 wire format).
//
// Feed it raw wire bytes; it collects 0x00-delimited COBS frames, decodes
// them, verifies the trailing CRC-8, strips the leading cobsSeq byte, and
// hands the (cobsSeq, payload) pair to a Listener. Bad CRC, malformed COBS,
// CRC-only frames, and oversize frames are reported as frame errors.
//
// Wire format (v4.0.0 — NOT interop-compatible with v3.x):
//   [0x00] [COBS(cobsSeq | payload) | CRC8(cobsSeq | payload)] [0x00]
// The first decoded byte of every reliable-mode frame is cobsSeq (8-bit,
// wraps at 256). The receiver uses it to drop stale or out-of-order
// frames so a wire-byte shift no longer desyncs the message layer.
//
// The keepalive is still the two-byte sequence 0x00 0x00. feed() consumes
// the pair at a clean boundary with no callback; mid-COBS the first 0x00
// closes the partial (one onFrameError) and the second is skipped. Stray
// single 0x00s are still skipped for back-compat.
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
    //
    // cobsSeq is the frame's sequence number (0..255, wraps). The receiver
    // uses it to drop stale or out-of-order frames before they reach the
    // message layer. Empty payload (n==0) is valid — it carries only the
    // cobsSeq byte (used for keepalive-style 0-byte data frames, if any).
    class Listener {
    public:
        virtual ~Listener() {}
        virtual bool onPayload(uint8_t cobsSeq, const uint8_t* b, int n) = 0; // validated frame
        virtual bool onFrameError() = 0;                                     // corrupt/oversize frame
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
