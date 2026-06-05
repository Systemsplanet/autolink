#include <iostream>
#include <cassert>
#include <queue>
#include <mutex>
#include <vector>
#include "ALink.h"

using namespace autolink;

class MockHal : public ILink {
public:
    uint32_t spd = 9600;
    bool timerActive = false;
    uint8_t txBuf[1024];
    int txN = 0;
    
    int sendBreakCalls = 0;
    int timerStartCalls = 0;
    std::vector<uint32_t> spdHistory;
    
    std::queue<uint8_t> appBuf;
    mutable std::mutex mtx; 
    
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
        uint8_t b = appBuf.front(); appBuf.pop();
        return b;
    }
    int peekAppBuf() override {
        if (appBuf.empty()) return -1;
        return appBuf.front();
    }
    int appBufAvailable() const override { return appBuf.size(); }
    void clearAppBuf() override { 
        while(!appBuf.empty()) appBuf.pop(); 
    }
};

void pipe_data(MockHal& src, MockHal& dest) {
    if (src.txN > 0) {
        dest.link->onRx(src.txBuf, src.txN);
        src.clearTx();
    }
}

void run_test_hal_methods() {
    std::cout << "\n=== Test: MockHal Methods ===" << std::endl;
    MockHal hal;
    hal.setSpd(115200); assert(hal.spd == 115200);
    hal.sendBreak(); assert(hal.sendBreakCalls == 1);
    
    uint8_t tb[] = {0xFF};
    hal.tx(tb, 1); assert(hal.txN == 1); assert(hal.txBuf[0] == 0xFF);
    hal.flushTx();
    
    hal.startTimer(50); assert(hal.timerActive == true);
    hal.stopTimer(); assert(hal.timerActive == false);
    
    hal.lock(); hal.unlock(); 
    
    hal.pushAppBuf(0xAA);
    assert(hal.appBufAvailable() == 1);
    assert(hal.peekAppBuf() == 0xAA);
    assert(hal.popAppBuf() == 0xAA);
    assert(hal.popAppBuf() == -1); 
    
    hal.pushAppBuf(0xBB);
    hal.clearAppBuf();
    assert(hal.appBufAvailable() == 0);
    
    std::cout << "PASS" << std::endl;
}

void run_test_basic_io() {
    std::cout << "\n=== Test: Basic Write/Read/Peek/Flush/Available ===" << std::endl;
    MockHal mHal, sHal;
    ALink master(mHal, true);
    ALink slave(sHal, false);
    
    uint8_t data[] = {0x11, 0x22};
    master.write(data, 2);
    master.flush();
    
    slave.onRx(mHal.txBuf, mHal.txN);
    
    assert(slave.available() == 2);
    assert(slave.peek() == 0x11);
    assert(slave.available() == 2);
    
    uint8_t rb_arr[10];
    assert(slave.read(rb_arr, 10) == 2);
    assert(rb_arr[0] == 0x11);
    assert(rb_arr[1] == 0x22);
    
    assert(slave.available() == 0);
    std::cout << "PASS" << std::endl;
}

void run_test_reliable_mode() {
    std::cout << "\n=== Test: Reliable Mode (COBS) ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg; cfg.reliableMode = true;
    ALink master(mHal, true, cfg);
    ALink slave(sHal, false, cfg);
    
    uint8_t data[] = {0xAA, 0xBB};
    master.write(data, 2);
    assert(mHal.txN > 0);
    
    slave.onRx(mHal.txBuf, mHal.txN);
    uint8_t rb_arr[10];
    assert(slave.read(rb_arr, 10) == 2);
    assert(rb_arr[0] == 0xAA);
    assert(rb_arr[1] == 0xBB);
    
    uint8_t bad_cobs[] = {0x00, 0x05, 0x01, 0x02, 0x00}; 
    slave.onRx(bad_cobs, sizeof(bad_cobs));
    assert(slave.getErrCount() > 0);
    
    std::cout << "PASS" << std::endl;
}

void run_test_error_threshold() {
    std::cout << "\n=== Test: Custom Error Thresholding ===" << std::endl;
    MockHal mHal;
    AutoLinkConfig cfg; cfg.allowedBauds = {9600, 115200}; cfg.errThreshold = 2;
    ALink master(mHal, true, cfg);
    
    assert(master.getState() == State::OK);
    master.err();
    assert(master.getState() == State::OK);
    assert(master.getErrCount() == 1);
    
    master.clearErr();
    assert(master.getErrCount() == 0);
    
    master.err();
    master.err();
    assert(master.getState() == State::OK);
    assert(master.getErrCount() == 2);
    
    master.err(); 
    assert(master.getState() == State::SWP);
    std::cout << "PASS" << std::endl;
}

void run_test_negotiation_state_machine() {
    std::cout << "\n=== Test: Auto-Baud Negotiation State Machine ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.allowedBauds = {9600, 115200};
    ALink master(mHal, true, cfg);
    ALink slave(sHal, false, cfg);
    
    master.onBreak();
    slave.onBreak();
    
    assert(master.getState() == State::SWP);
    assert(slave.getState() == State::SWP);
    assert(master.getCurrentSpdIndex() == 0);
    
    master.onTimer();
    pipe_data(mHal, sHal);
    assert(master.getCurrentSpdIndex() == 1);
    
    master.onTimer();
    pipe_data(mHal, sHal);
    assert(master.getCurrentSpdIndex() == 2);
    
    master.onTimer();
    assert(master.getState() == State::LCK);
    
    slave.onTimer(); slave.onTimer(); slave.onTimer();
    assert(slave.getState() == State::LCK);
    
    master.onTimer();
    pipe_data(mHal, sHal);
    assert(slave.getState() == State::OK);
    
    pipe_data(sHal, mHal);
    assert(master.getState() == State::OK);
    
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running AutoLink Core Tests ===" << std::endl;
    run_test_hal_methods();
    run_test_basic_io();
    run_test_reliable_mode();
    run_test_error_threshold();
    run_test_negotiation_state_machine();
    std::cout << "\n=== All Tests Completed Successfully ===" << std::endl;
    return 0;
}
