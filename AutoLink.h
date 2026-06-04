#pragma once
#include "EspHal.h"
#include "ALink.h"
#include <memory>

#ifdef ARDUINO
#include <Stream.h>
#else
// Mock Stream for native testing if ARDUINO is not defined
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
        hal = std::make_unique<EspHal>(u_num, rx_pin, tx_pin);
        link = std::make_unique<ALink>(*hal, isMasterNode, cfg);
    }

    bool isHealthy() const { return hal->isHealthy(); }
    
    // Standard Stream implementation
    int available() override { return link->available(); }
    int read() override { uint8_t b; return link->read(&b, 1) ? b : -1; }
    int peek() override { return link->peek(); }
    size_t write(uint8_t b) override { link->write(&b, 1); return 1; }
    size_t write(const uint8_t *buffer, size_t size) override { link->write(buffer, size); return size; }
    void flush() override { link->flush(); }
    
    // Standard array read
    int read(uint8_t* b, int max_len) { return link->read(b, max_len); }
    
    void err() { link->err(); }
    void clearErr() { link->clearErr(); }
    
    State getState() const { return link->getState(); }
};

} // namespace autolink
