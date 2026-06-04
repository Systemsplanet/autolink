#include <iostream>
#include <cassert>
#include <queue>
#include <mutex>
#include <vector>
#include "ALink.h"

// Enhanced Testable Mock Hardware Layer
class MockHal : public ILink {
public:
    uint32_t spd = 9600;
    bool timerActive = false;
    uint8_t txBuf[256];
    int txN = 0;
    
    // Spying Metrics for Advanced Testing
    int sendBreakCalls = 0;
    int timerStartCalls = 0;
    std::vector<uint32_t> spdHistory;
    
    std::queue<uint8_t> appBuf;
    mutable std::mutex mtx; // Mutable so const lock/unlock can access it
    
    void setSpd(uint32_t s) override { spd = s; spdHistory.push_back(s); }
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
    
    void lock() const override { mtx.lock(); }
    void unlock() const override { mtx.unlock(); }
    
    void pushAppBuf(uint8_t b) override { appBuf.push(b); }
    int popAppBuf() override {
        if (appBuf.empty()) return -1;
        uint8_t b = appBuf.front();
        appBuf.pop();
        return b;
    }
    int appBufAvailable() const override { return appBuf.size(); }
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

uint8_t calcTestCrc(const uint8_t* data, int len) {
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x07;
            else crc <<= 1;
        }
    }
    return crc;
}

void sendFrameTo(ALink& receiver, uint8_t payload) {
    uint8_t frame[4] = {0xAA, 0x55, payload, 0};
    frame[3] = calcTestCrc(frame, 3);
    receiver.onRx(frame, 4);
}

void run_test_basic_io() {
    std::cout << "Test: Basic Write/Read (Async Buffer)... ";
    MockHal mHal, sHal;
    ALink master(mHal, true);
    ALink slave(sHal, false);
    
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
    ALink master(mHal, true, {9600, 115200}, 2, 50);
    
    assert(master.getState() == State::OK);
    master.err();
    assert(master.getState() == State::OK);
    assert(master.getErrCount() == 1);
    master.err();
    assert(master.getState() == State::OK);
    assert(master.getErrCount() == 2);
    
    // 3rd error should cross the >2 threshold
    master.err();
    assert(master.getState() == State::SWP);
    assert(mHal.sendBreakCalls == 1);
    assert(mHal.spdHistory.back() == 9600);
    std::cout << "PASS" << std::endl;
}

void run_test_custom_config_sweep() {
    std::cout << "Test: Custom Baud Configuration & Sweep... ";
    MockHal mHal;
    ALink master(mHal, true, {1200, 2400, 4800}, 1, 10);
    
    master.err();
    master.err();
    assert(master.getState() == State::SWP);
    mHal.resetSpies();
    
    master.onTimer();
    assert(mHal.spdHistory.back() == 2400);
    
    master.onTimer();
    assert(mHal.spdHistory.back() == 4800); 
    
    master.onTimer();
    assert(master.getState() == State::LCK);
    assert(mHal.spdHistory.back() == 1200); 
    
    std::cout << "PASS" << std::endl;
}

void run_test_slave_score_selection() {
    std::cout << "Test: Slave Score Selection Logic... ";
    MockHal sHal;
    ALink slave(sHal, false, {9600, 19200, 38400}, 5, 50);
    
    sHal.sendBreak();
    assert(slave.getState() == State::SWP);
    
    sendFrameTo(slave, ALINK_PING_CMD);
    
    slave.onTimer();
    sendFrameTo(slave, ALINK_PING_CMD);
    sendFrameTo(slave, ALINK_PING_CMD);
    sendFrameTo(slave, ALINK_PING_CMD);
    
    slave.onTimer();
    sendFrameTo(slave, ALINK_PING_CMD);
    sendFrameTo(slave, ALINK_PING_CMD);
    
    slave.onTimer();
    assert(slave.getState() == State::LCK);
    
    sHal.clearTx();
    sendFrameTo(slave, ALINK_REQ_CMD);
    
    assert(sHal.txN == 4);
    assert(sHal.txBuf[2] == 1);
    assert(slave.getState() == State::OK);
    assert(sHal.spd == 19200);
    
    std::cout << "PASS" << std::endl;
}

void run_test_full_negotiation() {
    std::cout << "Test: Full Master/Slave Auto-Baud Lock... ";
    MockHal mHal, sHal;
    ALink master(mHal, true);
    ALink slave(sHal, false);
    
    for(int i=0; i<6; i++) master.err();
    sHal.sendBreak(); 
    
    for(int step=0; step<5; step++) {
        mHal.clearTx();
        master.onTimer(); 
        slave.onRx(mHal.txBuf, mHal.txN);
        slave.onTimer(); 
    }
    
    mHal.clearTx();
    master.onTimer();
    
    sHal.clearTx();
    slave.onRx(mHal.txBuf, mHal.txN);
    master.onRx(sHal.txBuf, sHal.txN);
    
    assert(slave.getState() == State::OK);
    assert(master.getState() == State::OK);
    assert(mHal.spd == 115200);
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
