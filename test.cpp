#include <iostream>
#include <cassert>
#include <queue>
#include <mutex>
#include <vector>
#include "ALink.h"

// Enhanced Testable Mock Hardware Layer
class MockHal : public ILink {
public:
    int spd = 9600;
    bool timerActive = false;
    uint8_t txBuf[256];
    int txN = 0;
    
    // Spying Metrics for Advanced Testing
    int sendBreakCalls = 0;
    int timerStartCalls = 0;
    std::vector<int> spdHistory;
    
    std::queue<uint8_t> appBuf;
    std::mutex mtx;
    
    void setSpd(int s) override { spd = s; spdHistory.push_back(s); }
    void sendBreak() override { 
        sendBreakCalls++; 
        if (link) link->onBreak(); 
    }
    void tx(const uint8_t* b, int n) override {
        for(int i=0; i<n; i++) txBuf[txN++] = b[i];
    }
    void flushTx() override {}
    void startTimer(int ms) override { timerStartCalls++; timerActive = true; }
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
    
    void resetSpies() {
        sendBreakCalls = 0;
        timerStartCalls = 0;
        spdHistory.clear();
        clearTx();
    }
};

void run_test_basic_io() {
    std::cout << "Test: Basic Write/Read (Async Buffer)... ";
    MockHal mHal, sHal;
    ALink master(&mHal, true);
    ALink slave(&sHal, false);
    
    uint8_t data[] = {0x11, 0x22};
    master.write(data, 2);
    slave.onRx(mHal.txBuf, mHal.txN);
    uint8_t rb_arr[10];
    assert(slave.read(rb_arr, 10) == 2);
    assert(rb_arr[0] == 0x11);
    assert(rb_arr[1] == 0x22);
    std::cout << "PASS" << std::endl;
}

void run_test_error_threshold() {
    std::cout << "Test: Custom Error Thresholding... ";
    MockHal mHal;
    // Set custom threshold to 2
    ALink master(&mHal, true, {9600, 115200}, 2, 50);
    
    assert(master.getState() == OK);
    master.err();
    assert(master.getState() == OK);
    assert(master.getErrCount() == 1);
    master.err();
    assert(master.getState() == OK);
    assert(master.getErrCount() == 2);
    
    // 3rd error should cross the >2 threshold
    master.err();
    assert(master.getState() == SWP);
    assert(mHal.sendBreakCalls == 1);
    assert(mHal.spdHistory.back() == 9600); // Transitions to 1st baud
    std::cout << "PASS" << std::endl;
}

void run_test_custom_config_sweep() {
    std::cout << "Test: Custom Baud Configuration & Sweep... ";
    MockHal mHal;
    ALink master(&mHal, true, {1200, 2400, 4800}, 1, 10);
    
    master.err();
    master.err(); // Triggers break
    assert(master.getState() == SWP);
    mHal.resetSpies();
    
    // Step 1: 1200
    master.onTimer();
    assert(mHal.spdHistory.back() == 2400); // advances to next
    
    // Step 2: 2400
    master.onTimer();
    assert(mHal.spdHistory.back() == 4800); 
    
    // Step 3: 4800 -> Finish Sweep
    master.onTimer();
    assert(master.getState() == LCK);
    assert(mHal.spdHistory.back() == 9600); // Always falls back to 9600 for LCK negotiation
    
    std::cout << "PASS" << std::endl;
}

void run_test_slave_score_selection() {
    std::cout << "Test: Slave Score Selection Logic... ";
    MockHal sHal;
    ALink slave(&sHal, false, {9600, 19200, 38400}, 5, 50);
    
    // Force slave into sweep
    sHal.sendBreak();
    assert(slave.getState() == SWP);
    
    // Simulate SpdI = 0 (9600) -> 1 ping
    uint8_t ping = 0x55;
    slave.onRx(&ping, 1);
    
    slave.onTimer(); // Advances to spdI = 1 (19200)
    // Send 3 pings
    slave.onRx(&ping, 1);
    slave.onRx(&ping, 1);
    slave.onRx(&ping, 1);
    
    slave.onTimer(); // Advances to spdI = 2 (38400)
    // Send 2 pings
    slave.onRx(&ping, 1);
    slave.onRx(&ping, 1);
    
    slave.onTimer(); // Advances to LCK
    assert(slave.getState() == LCK);
    
    // Master requests winner
    sHal.clearTx();
    uint8_t req = 0xAA;
    slave.onRx(&req, 1);
    
    // Slave should pick index 1 (19200) because it got 3 pings
    assert(sHal.txN == 1);
    assert(sHal.txBuf[0] == 1);
    assert(slave.getState() == OK);
    assert(sHal.spd == 19200); // verifies hal set speed correctly
    
    std::cout << "PASS" << std::endl;
}

void run_test_full_negotiation() {
    std::cout << "Test: Full Master/Slave Auto-Baud Lock... ";
    MockHal mHal, sHal;
    ALink master(&mHal, true);
    ALink slave(&sHal, false);
    
    // Trigger break
    for(int i=0; i<6; i++) master.err();
    sHal.sendBreak(); 
    
    // Process sweeps
    for(int step=0; step<5; step++) {
        mHal.clearTx();
        master.onTimer(); 
        slave.onRx(mHal.txBuf, mHal.txN);
        slave.onTimer(); 
    }
    
    // LCK phase
    mHal.clearTx();
    master.onTimer(); // Master sends req
    
    sHal.clearTx();
    slave.onRx(mHal.txBuf, mHal.txN); // Slave replies with index
    master.onRx(sHal.txBuf, sHal.txN); // Master locks
    
    assert(slave.getState() == OK);
    assert(master.getState() == OK);
    assert(mHal.spd == 115200); // Default array max
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running AutoLink Core Tests ===" << std::endl;
    run_test_basic_io();
    run_test_error_threshold();
    run_test_custom_config_sweep();
    run_test_slave_score_selection();
    run_test_full_negotiation();
    std::cout << "=== All Tests Completed Successfully ===" << std::endl;
    return 0;
}
