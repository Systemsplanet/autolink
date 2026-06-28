// Stream-byte COBS frame decoder.
// Type byte: 0xFF=ACK, 0xFE=NAK, 0x00-0xFD=data.
// 0xFE/0xFF are reserved and never used as seq.
//
// Wire ACK frame (5 bytes raw, COBS-encoded):
//   [0xFF, seq, bytes_lo, bytes_hi, frame_crc8]
// frame_crc8 is CRC-8 over bytes 0..3 (the existing
// convention). bytes_lo/hi carry the payload length the
// receiver just ACK'd; Ping reads them via the Link's
// bytesRecvd_ table so its matchEcho_ log line can show
// the actual bytes-recvd.
//
// Wire NAK frame (3 bytes raw, COBS-encoded):
//   [0xFE, seq, frame_crc8]
// The NAK seq is the expected (missing) cobsSeq, as before.
#pragma once
#include <stdint.h>

namespace autolink {
constexpr uint8_t ACK_TYPE = 0xFF;
constexpr uint8_t NAK_TYPE = 0xFE;
// Data seq wraps at 0xFD; skip 0xFE/0xFF.
constexpr uint8_t COBS_SEQ_MAX = 0xFD;

class UtilFrameRx {
public:
    class Listener {
    public:
        virtual ~Listener() {}
        virtual bool onPayload(uint8_t cobsSeq, const uint8_t *b, int n) = 0;
        // ACK carries the receiver-reported bytes-recvd in
        // addition to the acked seq. The link layer uses
        // this to populate bytesRecvd_[seq] so Ping's
        // matchEcho_ can log the actual payload length.
        virtual bool onAck(uint8_t ackedCobsSeq, uint16_t bytesRecvd) = 0;
        virtual bool onNak(uint8_t missingCobsSeq) {
            (void)missingCobsSeq;
            return false;
        }
        virtual bool onFrameError() = 0;
    };

    explicit UtilFrameRx(Listener &l) : lis(l) {}

    int feed(const uint8_t *data, int len);
    void reset();

private:
    Listener &lis;
    uint8_t buf[256];
    int idx = 0;
    uint8_t decoded[256];
};

} // namespace autolink
