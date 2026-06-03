#pragma once
#include "EspHal.h"
#include "ALink.h"
#include <memory>
#include <vector>

class AutoLink {
private:
    std::unique_ptr<EspHal> hal;
    std::unique_ptr<ALink> link;

public:
    AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMasterNode, 
             std::vector<int> allowedBauds = {9600, 19200, 38400, 57600, 115200}, 
             int errThresh = 5, 
             int delayMs = 50) 
    {
        hal.reset(new EspHal(u_num, rx_pin, tx_pin));
        link.reset(new ALink(hal.get(), isMasterNode, allowedBauds, errThresh, delayMs));
    }
    
    // No explicit destructor needed - std::unique_ptr ensures zero leaks.

    int available() { return link->available(); }
    int read(uint8_t* b, int max_len) { return link->read(b, max_len); }
    void write(const uint8_t* b, int len) { link->write(b, len); }
    void err() { link->err(); }
    State getState() { return link->getState(); }
};
