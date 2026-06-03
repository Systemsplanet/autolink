#pragma once
#include "EspHal.h"
#include "ALink.h"

class AutoLink {
private:
    EspHal* hal;
    ALink* link;

public:
    AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMaster) {
        hal = new EspHal(u_num, rx_pin, tx_pin);
        link = new ALink(hal, isMaster);
    }

    ~AutoLink() {
        delete link;
        delete hal;
    }

    int available() { return link->available(); }
    int read(uint8_t* b, int max_len) { return link->read(b, max_len); }
    void write(const uint8_t* b, int len) { link->write(b, len); }
    void err() { link->err(); }
    State getState() { return link->getState(); }
};
