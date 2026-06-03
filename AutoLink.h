#pragma once
#include "EspHal.h"
#include "ALink.h"

/**
 * AutoLink Wrapper Class
 * Provides a highly simplified API by automatically instantiating and 
 * managing the EspHal hardware driver and ALink state machine under the hood.
 */
class AutoLink {
private:
    EspHal* hal;
    ALink* link;

public:
    // Initialize UART natively with the specified pins and master/slave role
    AutoLink(uart_port_t u_num, int rx_pin, int tx_pin, bool isMaster) {
        hal = new EspHal(u_num, rx_pin, tx_pin);
        link = new ALink(hal, isMaster);
    }

    ~AutoLink() {
        delete link;
        delete hal;
    }

    // Check if safely-buffered bytes are available from the background task
    int available() { 
        return link->available(); 
    }

    // Read bytes into your buffer
    int read(uint8_t* b, int max_len) { 
        return link->read(b, max_len); 
    }

    // Write bytes natively over the UART
    void write(const uint8_t* b, int len) { 
        link->write(b, len); 
    }

    // Triggered by your upper protocol (Modbus, custom framing, etc.) on bad CRC
    void err() { 
        link->err(); 
    }

    // Get the current system state (OK, SWP, LCK)
    St getSt() { 
        return link->getSt(); 
    }
};
