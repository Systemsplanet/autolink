#include <iostream>
#include <cassert>
#include <queue>
#include <mutex>
#include "ALink.h"

class MockHal : public ILink {
public:
    int spd = 9600;
    bool timerActive = false;
    uint8_t txBuf[256];
    int txN = 0;
    
    std::queue<uint8_t> appBuf;
    std::mutex mtx;
    
    void setSpd(int s) override { spd = s; }
    void sendBreak() override { if (link) link->onBreak(); }
    void tx(const uint8_t* b, int n) override {
        for(int i=0; i<n; i++) txBuf[txN++] = b[i];
    }
    void flushTx() override {}
    void startTimer(int ms) override { timerActive = true; }
    void stopTimer() override { timerActive = false; }
    void clearTx() { txN = 0; }
    
    void lock() override { mtx.lock(); }
    void unlock() override { mtx.unlock(); }
    
    void pushAppBuf(uint8_t b) override { appBuf.push(b); }
    int popAppBuf() override {
        if (appBuf.empty()) return -1;
        uint8_t b = appBuf.front();
        appBuf.pop();
        return b;
    }
    int appBufAvailable() override { return appBuf.size(); }
    void clearAppBuf() override { 
        while(!appBuf.empty()) appBuf.pop(); 
    }
};

int main() {
    std::cout << "Test 0: Stream Buffer functionality (Mocked)" << std::endl;
    MockHal tHal;
    assert(tHal.appBufAvailable() == 0);
    tHal.pushAppBuf(0xAA);
    tHal.pushAppBuf(0xBB);
    assert(tHal.appBufAvailable() == 2);
    assert(tHal.popAppBuf() == 0xAA);
    assert(tHal.appBufAvailable() == 1);
    assert(tHal.popAppBuf() == 0xBB);
    assert(tHal.appBufAvailable() == 0);
    assert(tHal.popAppBuf() == -1);

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
