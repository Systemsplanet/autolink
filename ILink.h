#pragma once
#include <stdint.h>

class ALink;

// Hardware Abstraction Layer for event-driven UART
class ILink {
protected:
    ALink* link = nullptr;
public:
    virtual ~ILink() {}
    void bind(ALink* l) { link = l; }
    
    virtual void setSpd(int s) = 0;
    
    // Hardware native break transmission (No GPIO toggling)
    virtual void sendBreak() = 0;
    
    virtual void tx(const uint8_t* b, int n) = 0;
    virtual void flushTx() = 0;
    
    // Hardware Timer controls for the sweep state machine
    virtual void startTimer(int ms) = 0;
    virtual void stopTimer() = 0;
};
