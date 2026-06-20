#pragma once
#include <stdint.h>

// UtilFrameRx — reliable-mode receive accumulator.
// Feed raw wire bytes; collects 0x00-delimited COBS frames, verifies
// CRC-8, strips the leading cobsSeq, hands (cobsSeq, payload) to a
// Listener. Bad CRC, malformed COBS, oversize -> onFrameError.
//
// Wire format:
//   data:  [0x00] [COBS(cobsSeq | payload) | CRC8(cobsSeq|payload)] [0x00]
//   ack:   [0x00] [COBS(ACK_TYPE | ackedSeq) | CRC8(ACK_TYPE|ackedSeq)] [0x00]
// cobsSeq (8-bit, wraps 256) lets the receiver drop stale /
// out-of-order frames so a wire-byte shift doesn't desync the message
// layer.
//
// ACK_TYPE (0x33) as the first decoded byte marks an ACK frame — the
// second byte is the cobsSeq being acknowledged. Replaces the data
// path's cobsSeq slot; the receiver's onAck() callback is invoked
// instead of onPayload(). 5 wire bytes (2-byte payload + 1-byte CRC +
// 2-byte COBS overhead + 2 delimiter 0x00).
//
// Keepalive: 0x00 0x00 (consumed silently). Stray single 0x00s skipped.
//
// If the Listener reports a drop, feed() stops and returns how far it
// got so the caller can route the rest to its command parser. No
// locking, no allocation; thread safety is the caller's job.
// ----------------------------------------------------------------------------

namespace autolink {

// First decoded byte of an ACK frame. Distinct from any cobsSeq value
// the sender might emit (cobsSeq is 0..255 but ACK_TYPE = 0x33 is also
// in that range — receiver checks "first byte == ACK_TYPE" BEFORE
// interpreting as cobsSeq). On the wire, the receiver inspects only
// the first decoded byte; cobsSeq follows.
constexpr uint8_t ACK_TYPE = 0x33;

class UtilFrameRx {
public:
    // Listener — implemented by the owner (ALink) to receive validated
    // frame payloads, ACK notifications, and frame-error notifications.
    // All callbacks return true if the link was dropped mid-event so
    // feed() can stop early and hand the rest of the buffer to the
    // command parser.
    class Listener {
    public:
        virtual ~Listener() {}
        virtual bool onPayload(uint8_t cobsSeq, const uint8_t* b, int n) = 0; // validated data frame
        virtual bool onAck(uint8_t ackedCobsSeq) = 0;                         // validated ACK
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
