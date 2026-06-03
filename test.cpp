#include <iostream>
#include <cassert>
#include "ALink.h"

// Mock Hardware Interface
class MockHal : public ILink {
public:
    int spd = 9600;
    bool inBrk = false;
    uint32_t time = 0;
    uint8_t rxBuf[256];
    int rxN = 0;
    
    void setSpd(int s) override { spd = s; }
    void brk() override { inBrk = true; }
    int rx(uint8_t* b, int len) override {
        int n = std::min(len, rxN);
        for(int i=0; i<n; i++) b[i] = rxBuf[i];
        
        // shift remaining
        for(int i=n; i<rxN; i++) rxBuf[i-n] = rxBuf[i];
        rxN -= n;
        
        return n;
    }
    void tx(const uint8_t* b, int len) override {} // No-op for M mock
    uint32_t ms() override { return time; }
    
    void pushRx(uint8_t val) { rxBuf[rxN++] = val; }
};

int main() {
    MockHal hw;
    ALink link(&hw, true); // Initialize as Master
    
    std::cout << "Starting tests..." << std::endl;
    assert(link.getSt() == OK);
    
    // Simulate upper layer CRC errors
    std::cout << "Testing Error Trigger..." << std::endl;
    for(int i=0; i<6; i++) link.err();
    assert(link.getSt() == BRK);
    
    // Process break
    std::cout << "Testing Break State..." << std::endl;
    hw.time = 150; 
    link.tick();
    assert(link.getSt() == SWP);
    assert(hw.spd == 9600);
    
    // Process Sweep Array (50ms per step)
    std::cout << "Testing Baud Sweep State..." << std::endl;
    hw.time += 60; link.tick(); assert(hw.spd == 19200);
    hw.time += 60; link.tick(); assert(hw.spd == 38400);
    hw.time += 60; link.tick(); assert(hw.spd == 57600);
    hw.time += 60; link.tick(); assert(hw.spd == 115200);
    
    // Move to Lock State
    std::cout << "Testing Lock State Negotiation..." << std::endl;
    hw.time += 60; link.tick(); 
    assert(link.getSt() == LCK);
    assert(hw.spd == 9600); // Must drop to 9600 to negotiate
    
    // Simulate Slave replying with Index 3 (57600 baud)
    hw.pushRx(3);
    link.tick();
    
    assert(link.getSt() == OK);
    assert(hw.spd == 57600); // Successfully jumped to new agreed speed
    
    std::cout << "All AutoLink Unit Tests Passed Successfully!" << std::endl;
    return 0;
}
