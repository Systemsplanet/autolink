#pragma once
#include "EspHal.h"
#include "ALink.h"
#include "Log.h"
#include <memory>

#ifdef ARDUINO
#include <Stream.h>
#else
class Stream {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual size_t write(uint8_t) = 0;
    virtual size_t write(const uint8_t *buffer, size_t size) = 0;
    virtual void flush() = 0;
};
#endif

namespace autolink {

class AutoLink : public Stream {
private:
    std::unique_ptr<EspHal> hal;
    std::unique_ptr<ALink> link;

public:
    AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMasterNode, const AutoLinkConfig& cfg = AutoLinkConfig())
    {
        hal = std::make_unique<EspHal>(u_num, rx_pin, tx_pin, cfg);
        link = std::make_unique<ALink>(*hal, isMasterNode, cfg);
    }

    void begin() { hal->begin(); }

    bool isHealthy() const { return hal->isHealthy(); }

    // ---- Stream byte API ----
    int available() override { return link->available(); }
    int read() override { return link->read(); }
    int peek() override { return link->peek(); }
    size_t write(uint8_t b) override { return link->write(&b, 1); }
    size_t write(const uint8_t *buffer, size_t size) override { return link->write(buffer, (int)size); }
    void flush() override { link->flush(); }
    int read(uint8_t* b, int max_len) { return link->read(b, max_len); }

    // ---- Message API (boundary-preserving, CRC16-checked) ----
    bool sendMsg(const uint8_t* b, int len) { return link->sendMsg(b, len); }
    int  recvMsg(uint8_t* b, int max_len)   { return link->recvMsg(b, max_len); }

    // ---- Throughput ----
    void getStats(uint64_t& tx, uint64_t& rx) const { link->getStats(tx, rx); }
    void resetStats() { link->resetStats(); }

    // ---- Error / state ----
    void err() { link->err(); }
    void clearErr() { link->clearErr(); }
    int  getErrCount() const { return link->getErrCount(); }
    State getState() const { return link->getState(); }
};

} // namespace autolink
