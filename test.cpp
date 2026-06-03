#include <iostream>
#include <cassert>
#include "ALink.h"

class MockHal : public ILink {
public:
    int spd = 9600;
    bool timerActive = false;
    uint8_t txBuf[256];
    int txN = 0;
    
    void setSpd(int s) override { spd = s; }
    void sendBreak() override { if (link) link->onBreak(); }
    void tx(const uint8_t* b, int n) override {
        for(int i=0; i<n; i++) txBuf[txN++] = b[i];
    }
    void flushTx() override {}
    void startTimer(int ms) override { timerActive = true; }
    void stopTimer() override { timerActive = false; }
    void clearTx() { txN = 0; }
};

int main() {
    // Tests remain coupled directly to the ALink core to prove the 
    // state machine works entirely without ESP32 hardware dependencies.
    MockHal mHal, sHal;
    ALink master(&mHal, true);
    ALink slave(&sHal, false);
    
    std::cout << "Test 1: Core Write/Read (Async Buffer)" << std::endl;
    uint8_t data[] = {0x11, 0x22};
    master.write(data, 2);
    slave.onRx(mHal.txBuf, mHal.txN);
    uint8_t rb[10];
    assert(slave.read(rb, 10) == 2 && rb[0] == 0x11);
    
    std::cout << "Test 2: Trigger CRC Error -> Hardware Break" << std::endl;
    for(int i=0; i<6; i++) master.err();
    assert(master.getSt() == SWP); 
    
    sHal.sendBreak(); 
    assert(slave.getSt() == SWP);
    
    std::cout << "Test 3: Sweep Phase (Timer Driven)" << std::endl;
    for(int step=0; step<5; step++) {
        mHal.clearTx();
        master.onTimer(); 
        slave.onRx(mHal.txBuf, 1);
        slave.onTimer(); 
    }
    
    std::cout << "Test 4: Lock Phase Negotiation" << std::endl;
    mHal.clearTx();
    master.onTimer();
    
    sHal.clearTx();
    slave.onRx(mHal.txBuf, 1);
    
    assert(slave.getSt() == OK);
    master.onRx(sHal.txBuf, 1);
    assert(master.getSt() == OK);
    assert(mHal.spd == 115200);
    
    std::cout << "SUCCESS: All Fully-Asynchronous AutoLink Core Tests Passed!" << std::endl;
    return 0;
}
