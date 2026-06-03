#include <iostream>
#include <cassert>
#include "ALink.h"
#include "RingBuffer.h"

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
    std::cout << "Test 0: RingBuffer functionality" << std::endl;
    RingBuffer rb;
    assert(rb.available() == 0);
    rb.push(0xAA);
    rb.push(0xBB);
    assert(rb.available() == 2);
    assert(rb.pop() == 0xAA);
    assert(rb.available() == 1);
    assert(rb.pop() == 0xBB);
    assert(rb.available() == 0);
    assert(rb.pop() == -1); // empty

    MockHal mHal, sHal;
    ALink master(&mHal, true);
    ALink slave(&sHal, false);
    
    std::cout << "Test 1: Core Write/Read (Async Buffer)" << std::endl;
    uint8_t data[] = {0x11, 0x22};
    master.write(data, 2);
    slave.onRx(mHal.txBuf, mHal.txN);
    uint8_t rb_arr[10];
    assert(slave.read(rb_arr, 10) == 2 && rb_arr[0] == 0x11);
    
    std::cout << "Test 2: Trigger CRC Error -> Hardware Break" << std::endl;
    for(int i=0; i<6; i++) master.err();
    assert(master.getState() == SWP); 
    
    sHal.sendBreak(); 
    assert(slave.getState() == SWP);
    
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
    
    assert(slave.getState() == OK);
    master.onRx(sHal.txBuf, 1);
    assert(master.getState() == OK);
    assert(mHal.spd == 115200);
    
    std::cout << "SUCCESS: All Fully-Asynchronous AutoLink Core Tests Passed!" << std::endl;
    return 0;
}
