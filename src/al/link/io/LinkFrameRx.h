
#pragma once
#include <stdint.h>

namespace autolink {
constexpr uint8_t ACK_TYPE = 0xFF;
constexpr uint8_t NAK_TYPE = 0xFE;

constexpr uint8_t COBS_SEQ_MAX = 0xFD;

class UtilFrameRx {
public:
    class Listener {
    public:
        virtual ~Listener() {}
        virtual bool onPayload(uint8_t cobsSeq, const uint8_t *b, int n) = 0;

        virtual bool onAck(uint8_t ackedCobsSeq, uint16_t bytesRecvd) = 0;
        virtual bool onNak(uint8_t missingCobsSeq) = 0;
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
