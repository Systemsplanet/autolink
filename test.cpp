#include <iostream>
#include <cassert>
#include "ALink.h"

// Simulate the ESP32 Hardware Environment perfectly on a PC
class MockHal : public ILink {
public:
    int spd = 9600;
    bool timerActive = false;
    uint8_t txBuf[256];
    int txN = 0;
    
    void setSpd(int s) override { spd = s; }
    void sendBreak() override { 
        if (link) link->onBreak(); 
    }
    void tx(const uint8_t* b, int n) override {
        for(int i=0; i<n; i++) txBuf[txN++] = b[i];
    }
    void flushTx() override {}
    void startTimer(int ms) override { timerActive = true; }
    void stopTimer() override { timerActive = false; }
    
    void clearTx() { txN = 0; }
};

int main() {
    MockHal mHal, sHal;
    ALink master(&mHal, true);
    ALink slave(&sHal, false);
    
    std::cout << "Test 1: App Layer Write/Read (Async Buffer)" << std::endl;
    uint8_t data[] = {0x11, 0x22};
    master.write(data, 2);
    assert(mHal.txN == 2);
    slave.onRx(mHal.txBuf, mHal.txN);
    uint8_t rb[10];
    int rn = slave.read(rb, 10);
    assert(rn == 2 && rb[0] == 0x11);
    
    std::cout << "Test 2: Trigger CRC Error -> Hardware Break" << std::endl;
    for(int i=0; i<6; i++) master.err();
    assert(master.getSt() == SWP); 
    assert(mHal.spd == 9600);
    assert(mHal.timerActive);
    
    // Simulate UART hardware natively detecting the break at slave
    sHal.sendBreak(); 
    assert(slave.getSt() == SWP);
    
    std::cout << "Test 3: Sweep Phase (Timer Driven)" << std::endl;
    for(int step=0; step<5; step++) {
        mHal.clearTx();
        master.onTimer(); 
        assert(mHal.txN == 1 && mHal.txBuf[0] == 0x55); // Master sent Ping
        
        slave.onRx(mHal.txBuf, 1); // Slave captured Ping automatically via ISR
        slave.onTimer(); 
    }
    
    std::cout << "Test 4: Lock Phase Negotiation" << std::endl;
    assert(master.getSt() == LCK);
    assert(slave.getSt() == LCK);
    
    mHal.clearTx();
    master.onTimer(); // Master timer triggers request packet
    assert(mHal.txN == 1 && mHal.txBuf[0] == 0xAA);
    
    sHal.clearTx();
    slave.onRx(mHal.txBuf, 1); // Slave processes request
    
    assert(slave.getSt() == OK);
    assert(sHal.txN == 1); // Slave responds with Index 4 (115200)
    assert(sHal.spd == 115200);
    
    master.onRx(sHal.txBuf, 1); // Master processes response
    assert(master.getSt() == OK);
    assert(mHal.spd == 115200);
    
    std::cout << "SUCCESS: All Fully-Asynchronous AutoLink Tests Passed!" << std::endl;
    return 0;
}
