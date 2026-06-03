#pragma once
#include <stdint.h>

// Hardware Abstraction Layer for easy testing on PC or ESP32
class ILink {
public:
    virtual void setSpd(int s) = 0;
    virtual void brk() = 0; // Send hardware break
    virtual int rx(uint8_t* b, int n) = 0;
    virtual void tx(const uint8_t* b, int n) = 0;
    virtual uint32_t ms() = 0; // Milliseconds timer
};
