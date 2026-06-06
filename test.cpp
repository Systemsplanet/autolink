// Host-only unit tests. Arduino/ESP32 builds skip this entire file.
#ifndef ARDUINO

#include <iostream>
#include <iomanip>
#include <chrono>
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
    std::vector<uint8_t> txBuf; 
    
    int sendBreakCalls = 0;
    int timerStartCalls = 0;
    std::vector<uint32_t> spdHistory;
    
    std::queue<uint8_t> appBuf;
    mutable std::mutex mtx; 
    
    void begin() override {}
    void setSpd(uint32_t s) override { spd = s; spdHistory.push_back(s); }
    void sendBreak() override { 
        sendBreakCalls++; 
        if (link) link->onBreak(); 
    }
    void tx(const uint8_t* b, int n) override {
        txBuf.insert(txBuf.end(), b, b+n);
    }
    void flushTx() override {}
    void startTimer(int ms) override { timerStartCalls++; timerActive = true; }
    void stopTimer() override { timerActive = false; }
    void delayMs(int ms) override {}
    void clearTx() { txBuf.clear(); }
    
    void lock() const override { mtx.lock(); }
    void unlock() const override { mtx.unlock(); }
    
    void pushAppBuf(uint8_t b) override { appBuf.push(b); }
    void pushAppBuf(const uint8_t* b, int n) override {
        for(int i=0; i<n; i++) appBuf.push(b[i]);
    }
    int popAppBuf() override {
        if (appBuf.empty()) return -1;
        uint8_t b = appBuf.front(); appBuf.pop();
        return b;
    }
    int popAppBuf(uint8_t* b, int max_len) override {
        int n = 0;
        while(n < max_len && !appBuf.empty()) {
            b[n++] = appBuf.front();
            appBuf.pop();
        }
        return n;
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
    if (!src.txBuf.empty()) {
        dest.link->onRx(src.txBuf.data(), src.txBuf.size());
        src.clearTx();
    }
}

void run_test_hal_methods() {
    std::cout << "\n=== Test: MockHal Methods ===" << std::endl;
    MockHal hal;
    hal.setSpd(115200); assert(hal.spd == 115200);
    hal.sendBreak(); assert(hal.sendBreakCalls == 1);
    
    uint8_t tb[] = {0xFF};
    hal.tx(tb, 1); assert(hal.txBuf.size() == 1); assert(hal.txBuf[0] == 0xFF);
    hal.flushTx();
    
    hal.startTimer(50); assert(hal.timerActive == true);
    hal.stopTimer(); assert(hal.timerActive == false);
    
    hal.lock(); hal.unlock(); 
    
    uint8_t pb[] = {0xAA, 0xBB};
    hal.pushAppBuf(pb, 2);
    assert(hal.appBufAvailable() == 2);
    assert(hal.peekAppBuf() == 0xAA);
    
    uint8_t rb[2];
    assert(hal.popAppBuf(rb, 2) == 2);
    assert(rb[0] == 0xAA && rb[1] == 0xBB);
    assert(hal.popAppBuf() == -1); 
    
    hal.pushAppBuf(0xCC);
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
    
    slave.onRx(mHal.txBuf.data(), mHal.txBuf.size());
    
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
    assert(!mHal.txBuf.empty());
    
    slave.onRx(mHal.txBuf.data(), mHal.txBuf.size());
    uint8_t rb_arr[10];
    assert(slave.read(rb_arr, 10) == 2);
    assert(rb_arr[0] == 0xAA);
    assert(rb_arr[1] == 0xBB);
    
    // Craft a valid COBS frame but with a wrong CRC byte so the receiver calls err().
    // Payload: {0x01, 0x02}, correct CRC appended, then we flip the CRC.
    // COBS encode of {0x01, 0x02, bad_crc}: all non-zero -> {0x04, 0x01, 0x02, bad_crc}
    // Frame on wire: 0x00 0x04 0x01 0x02 0xFF 0x00  (0xFF is the deliberately wrong CRC)
    uint8_t bad_crc_frame[] = {0x00, 0x04, 0x01, 0x02, 0xFF, 0x00};
    slave.onRx(bad_crc_frame, sizeof(bad_crc_frame));
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

    master.begin();
    slave.begin();

    assert(master.getState() == State::SWP);
    assert(slave.getState() == State::SWP);
    assert(master.getCurrentSpdIndex() == 0);

    // Master sweeps: 2 bauds -> 2 PING ticks then LCK
    master.onTimer();
    pipe_data(mHal, sHal); // slave scores PING@9600
    assert(master.getCurrentSpdIndex() == 1);

    master.onTimer();
    pipe_data(mHal, sHal); // slave scores PING@115200, master -> LCK
    assert(master.getCurrentSpdIndex() == 2);

    assert(master.getState() == State::LCK);

    // Master sends REQ_CMD; slave handles it directly from SWP -> OK
    master.onTimer();
    pipe_data(mHal, sHal);
    assert(slave.getState() == State::OK);

    // Slave reply (baud index) arrives at master -> OK
    pipe_data(sHal, mHal);
    assert(master.getState() == State::OK);
    
    std::cout << "PASS" << std::endl;
}

void run_test_throughput_and_sizes() {
    std::cout << "\n=== Test: Payloads & Throughput (Reliable Mode) ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg; 
    cfg.reliableMode = true; 
    cfg.streamBufferSize = 32000; 
    ALink master(mHal, true, cfg);
    ALink slave(sHal, false, cfg);
    
    std::vector<int> sizes = {0, 1, 2, 4, 8, 16, 32, 64, 128, 512, 1024, 2048, 4096, 8000, 16000};
    
    std::cout << std::left << std::setw(15) << "Payload Size" 
              << std::setw(20) << "Time Taken (s)" 
              << std::setw(20) << "Bytes/Sec" << std::endl;
    std::cout << std::string(55, '-') << std::endl;

    for(int sz : sizes) {
        std::vector<uint8_t> txData(sz > 0 ? sz : 1);
        std::vector<uint8_t> rxData(sz > 0 ? sz : 1);
        
        for(int i=0; i<sz; i++) {
            txData[i] = i & 0xFF; // Fill with dummy data
        }
        
        auto start = std::chrono::high_resolution_clock::now();
        
        if (sz > 0) {
            master.write(txData.data(), sz);
        }
        
        pipe_data(mHal, sHal);
        
        int bytesRead = 0;
        if (sz > 0) {
            // Read until all bytes are consumed
            int chunk;
            while ((chunk = slave.read(rxData.data() + bytesRead, sz - bytesRead)) > 0) {
                bytesRead += chunk;
            }
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = end - start;
        
        double bps = sz > 0 ? (sz / diff.count()) : 0.0;
        
        assert(bytesRead == sz);
        if(sz > 0) {
            for(int i=0; i<sz; i++) {
                if (rxData[i] != txData[i]) {
                    std::cerr << "Data mismatch at index " << i << " for size " << sz << std::endl;
                    assert(false);
                }
            }
        }
        
        std::cout << std::left << std::setw(15) << sz 
                  << std::setw(20) << std::fixed << std::setprecision(6) << diff.count() 
                  << std::setw(20) << std::fixed << std::setprecision(2) << bps << std::endl;
    }
    
    std::cout << "\nPASS" << std::endl;
}

void run_test_readme_usage() {
    std::cout << "\n=== Test: Real-world README Usage Simulation ===" << std::endl;

    // --- Setup Phase ---
    AutoLinkConfig cfg;
    cfg.reliableMode = true;
    cfg.streamBufferSize = 2048;
    cfg.allowedBauds = {9600, 115200}; // 2 bauds for deterministic negotiation

    MockHal txHal, rxHal;
    ALink txNode(txHal, true, cfg);
    ALink link(rxHal, false, cfg);

    // begin() sets master to SWP (via sendBreak->onBreak and direct onBreak call).
    // MockHal::sendBreak() calls onBreak() internally, so master hits onBreak twice;
    // this is idempotent — second call just resets the same SWP state.
    txNode.begin(); // master: SWP, timer armed
    link.begin();   // slave:  SWP, no timer

    // Fast-forward negotiation to OK.
    // With 2 bauds: 2 SWP timer ticks send PINGs (spdI 0->1->2 -> LCK), then
    // 1 LCK tick sends REQ_CMD. Slave handles REQ from SWP directly -> OK.
    txNode.onTimer(); pipe_data(txHal, rxHal); // SWP: PING@9600, spdI->1
    txNode.onTimer(); pipe_data(txHal, rxHal); // SWP: PING@115200, spdI->2 -> LCK
    txNode.onTimer(); pipe_data(txHal, rxHal); // LCK: REQ_CMD; slave -> OK, replies index
    pipe_data(rxHal, txHal);                   // master receives baud index -> OK

    assert(txNode.getState() == State::OK);
    assert(link.getState()   == State::OK);

    // --- Execution Phase ---
    // Simulate master sending 3 bytes
    uint8_t payload[] = {0xAB, 0xCD, 0xEF};
    txNode.write(payload, 3);

    // Simulate UART RX interrupt delivering bytes to slave
    pipe_data(txHal, rxHal);

    // --- Loop Phase ---
    int bytes_processed = 0;
    while (link.available()) {
        int b = link.read();
        std::cout << "Got: " << std::hex << std::uppercase
                  << std::setw(2) << std::setfill('0') << b << std::dec << std::endl;
        assert(b == payload[bytes_processed]);
        bytes_processed++;
    }

    assert(bytes_processed == 3);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running AutoLink Core Tests ===" << std::endl;
    run_test_hal_methods();
    run_test_basic_io();
    run_test_reliable_mode();
    run_test_error_threshold();
    run_test_negotiation_state_machine();
    run_test_throughput_and_sizes();
    run_test_readme_usage();
    std::cout << "\n=== All Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif // ARDUINO
