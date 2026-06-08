// Host-only unit tests. Arduino/ESP32 builds skip this entire file.
#ifndef ARDUINO

#include <iostream>
#include <iomanip>
#include <chrono>
#include <cassert>
#include <queue>
#include <mutex>
#include <vector>
#include <cstdlib>
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
    AutoLinkConfig cfg; cfg.reliableMode = false; // this test exercises the raw byte path
    ALink master(mHal, true, cfg);
    ALink slave(sHal, false, cfg);
    // Both nodes start in State::OK by constructor default. begin() is deliberately
    // not called here so this test exercises only the data path in isolation,
    // without negotiation. This mirrors a known-good-baud scenario (e.g. fixed config).
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

    // master.begin() -> MockHal::sendBreak() -> onBreak() [exactly once].
    // slave.begin()  -> arms SWP passively, no timer.
    master.begin();
    slave.begin();

    assert(master.getState() == State::SWP);
    assert(slave.getState() == State::SWP);
    assert(master.getCurrentSpdIndex() == 0);

    // Tick 1: master sends PING@9600, slave scores it into scores[0], slave spdI->1
    master.onTimer();
    pipe_data(mHal, sHal);
    assert(master.getCurrentSpdIndex() == 1);
    assert(slave.getCurrentSpdIndex() == 1); // slave advanced after scoring

    // Tick 2: master sends PING@115200, slave scores into scores[1], slave spdI->2, master -> LCK
    master.onTimer();
    pipe_data(mHal, sHal);
    assert(master.getCurrentSpdIndex() == 2);
    assert(master.getState() == State::LCK);

    // Tick 3: master sends REQ_CMD; slave handles from SWP -> OK, replies best index
    master.onTimer();
    pipe_data(mHal, sHal);
    assert(slave.getState() == State::OK);

    // Master receives slave's baud-index reply -> OK
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

    txNode.begin(); // master: MockHal::sendBreak() -> onBreak() [once], timer armed
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

void run_test_message_roundtrip() {
    std::cout << "\n=== Test: Message API Round-Trip (random sizes) ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.reliableMode = true;
    cfg.streamBufferSize = 70000;
    cfg.maxMsg = 65535;
    ALink a(mHal, true, cfg);
    ALink b(sHal, false, cfg);

    srand(1234);
    std::vector<int> sizes = {1, 2, 3, 7, 250, 251, 500, 1000, 4096, 9001, 65535};
    for (int sz : sizes) {
        std::vector<uint8_t> tx(sz), rx(sz + 16);
        for (int i = 0; i < sz; i++) tx[i] = (uint8_t)(rand() & 0xFF);

        assert(a.sendMsg(tx.data(), sz));
        pipe_data(mHal, sHal);

        int got = b.recvMsg(rx.data(), rx.size());
        assert(got == sz);
        for (int i = 0; i < sz; i++) assert(rx[i] == tx[i]);
        // boundary preserved: nothing extra waiting
        assert(b.recvMsg(rx.data(), rx.size()) == 0);
    }
    std::cout << "PASS" << std::endl;
}

void run_test_message_boundaries_back_to_back() {
    std::cout << "\n=== Test: Back-to-Back Messages Keep Boundaries ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
    ALink a(mHal, true, cfg);
    ALink b(sHal, false, cfg);

    uint8_t m1[] = {1, 2, 3};
    uint8_t m2[] = {9, 8, 7, 6, 5};
    assert(a.sendMsg(m1, 3));
    assert(a.sendMsg(m2, 5));
    pipe_data(mHal, sHal); // both messages arrive together

    uint8_t rx[32];
    assert(b.recvMsg(rx, sizeof(rx)) == 3);
    assert(rx[0] == 1 && rx[2] == 3);
    assert(b.recvMsg(rx, sizeof(rx)) == 5);
    assert(rx[0] == 9 && rx[4] == 5);
    assert(b.recvMsg(rx, sizeof(rx)) == 0);
    std::cout << "PASS" << std::endl;
}

// Integration sweep across the sizes the README promises to support. Covers
// three distinct classes of stress in one pass:
//
//   * Boundary framing: 1..10 B exercise single-frame COBS with very short
//     payloads, where the inner cobsDecode loop is fed a 1- or 2-byte run.
//   * Cross-chunk reassembly: 1000, 2000 B force 4 and 8 MAX_CHUNK=250 frames
//     in flight together, exercising the message reassembly state machine.
//   * Large-payload stress: 10000 B is 40 frames, big enough that a missing
//     memcpy or off-by-one in the chunker would corrupt the tail.
//
// Every iteration uses a distinct fill byte so a cross-message leak in the
// reassembly buffer would surface as a payload mismatch on the next recv
// (the existing single-message tests can't catch that). At each size we
// also send a back-to-back different-sized message to verify the receiver
// keeps the message boundary after the largest payloads, and we send a
// small-large-small sequence at the top size to exercise the parser across
// a multi-message burst at the same buffer occupancy.
void run_test_message_size_sweep() {
    std::cout << "\n=== Test: Message API Size Sweep (0..10000) ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.reliableMode = true;
    cfg.streamBufferSize = 131072;   // 10000 + header + slack
    cfg.maxMsg = 65535;
    ALink a(mHal, true, cfg);
    ALink b(sHal, false, cfg);

    // Zero-length: documented as rejected. Verify before we begin the sweep
    // so a regression in the length guard can't masquerade as a send success.
    {
        uint8_t scratch[1] = {0};
        bool sent = a.sendMsg(scratch, 0);
        assert(sent == false);
        // No wire activity should have happened.
        assert(mHal.txBuf.empty());
        std::cout << "  [0]    rejected as expected" << std::endl;
    }

    // Reset for the rest of the test.
    mHal.txBuf.clear();
    uint64_t expectedTx = 0, expectedRx = 0;

    const std::vector<int> sizes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 100, 1000, 2000, 10000};
    const std::vector<int> tailSizes = {3, 250, 7};  // mixed back-to-back per size

    for (size_t idx = 0; idx < sizes.size(); idx++) {
        const int sz = sizes[idx];
        const uint8_t fill = (uint8_t)(0xA0 ^ (uint8_t)idx);  // unique per iteration
        std::vector<uint8_t> tx(sz), rx(sz + 32);
        for (int i = 0; i < sz; i++) tx[i] = (uint8_t)(fill + i);

        // Primary message.
        assert(a.sendMsg(tx.data(), sz));
        // Tail message of a different size, different fill, sent back-to-back
        // before the wire is drained. Catches any message-boundary loss after
        // a multi-frame primary.
        int tailSz = tailSizes[idx % tailSizes.size()];
        uint8_t tailFill = (uint8_t)(fill ^ 0x5A);
        std::vector<uint8_t> tailTx(tailSz), tailRx(tailSz + 32);
        for (int i = 0; i < tailSz; i++) tailTx[i] = (uint8_t)(tailFill + i);
        assert(a.sendMsg(tailTx.data(), tailSz));

        pipe_data(mHal, sHal);

        // Receive primary.
        int got = b.recvMsg(rx.data(), (int)rx.size());
        if (got != sz) {
            std::cerr << "  size=" << sz << " expected " << sz << " got " << got << std::endl;
            assert(false);
        }
        for (int i = 0; i < sz; i++) {
            if (rx[i] != tx[i]) {
                std::cerr << "  size=" << sz << " payload mismatch at i=" << i
                          << " (expected 0x" << std::hex << (int)tx[i]
                          << " got 0x" << (int)rx[i] << std::dec << ")" << std::endl;
                assert(false);
            }
        }

        // Receive tail. Boundary must be intact.
        int gotTail = b.recvMsg(tailRx.data(), (int)tailRx.size());
        if (gotTail != tailSz) {
            std::cerr << "  size=" << sz << " tail size " << tailSz
                      << " expected got=" << gotTail << std::endl;
            assert(false);
        }
        for (int i = 0; i < tailSz; i++) assert(tailRx[i] == tailTx[i]);

        // Nothing extra waiting.
        uint8_t probe[16];
        assert(b.recvMsg(probe, sizeof(probe)) == 0);

        expectedTx += (uint64_t)(sz + tailSz) + (uint64_t)MSG_HDR * 2;
        expectedRx += (uint64_t)(sz + tailSz) + (uint64_t)MSG_HDR * 2;

        std::cout << "  [" << sz << " B / tail " << tailSz << " B] ok"
                  << "  (txBuf wire bytes so far: " << mHal.txBuf.size() << ")"
                  << std::endl;
    }

    // Multi-message burst at the top size: send small, large, small in a
    // single onRx delivery. Verifies the parser stays aligned across the
    // most stressful occupancy (3 messages, middle one is 40 frames).
    {
        const uint8_t fA = 0x11, fB = 0x22, fC = 0x33;
        std::vector<uint8_t> mA(3), mB(10000), mC(7);
        std::vector<uint8_t> rA(3 + 16), rB(10000 + 32), rC(7 + 16);
        for (int i = 0; i < 3; i++)    mA[i] = (uint8_t)(fA + i);
        for (int i = 0; i < 10000; i++) mB[i] = (uint8_t)(fB + i);
        for (int i = 0; i < 7; i++)    mC[i] = (uint8_t)(fC + i);

        assert(a.sendMsg(mA.data(), 3));
        assert(a.sendMsg(mB.data(), 10000));
        assert(a.sendMsg(mC.data(), 7));
        pipe_data(mHal, sHal);

        assert(b.recvMsg(rA.data(), (int)rA.size()) == 3);
        for (int i = 0; i < 3; i++) assert(rA[i] == mA[i]);
        assert(b.recvMsg(rB.data(), (int)rB.size()) == 10000);
        for (int i = 0; i < 10000; i++) {
            if (rB[i] != mB[i]) {
                std::cerr << "  burst: B mismatch at i=" << i << std::endl;
                assert(false);
            }
        }
        assert(b.recvMsg(rC.data(), (int)rC.size()) == 7);
        for (int i = 0; i < 7; i++) assert(rC[i] == mC[i]);
        assert(b.recvMsg(rA.data(), (int)rA.size()) == 0);
        std::cout << "  [burst: 3 + 10000 + 7] ok" << std::endl;
    }

    // Stats sanity. txBytes / rxBytes are payload-stream counters on each
    // side, and they should match exactly across the wire. The burst above
    // adds 3 + 10000 + 7 payload bytes plus 3 MSG_HDRs on top of the sweep.
    expectedTx += (uint64_t)(3 + 10000 + 7) + (uint64_t)MSG_HDR * 3;
    expectedRx += (uint64_t)(3 + 10000 + 7) + (uint64_t)MSG_HDR * 3;
    uint64_t atx, arx, btx, brx;
    a.getStats(atx, arx);
    b.getStats(btx, brx);
    assert(atx == expectedTx);
    assert(brx == expectedRx);
    assert(atx == brx);
    std::cout << "  [stats] sender tx=" << atx << " receiver rx=" << brx
              << " (expected " << expectedTx << ")" << std::endl;

    std::cout << "PASS" << std::endl;
}

void run_test_message_crc_reject() {
    std::cout << "\n=== Test: Corrupt Message Rejected (CRC16) ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
    ALink a(mHal, true, cfg);
    ALink b(sHal, false, cfg);

    uint8_t msg[] = {0x10, 0x20, 0x30, 0x40};
    assert(a.sendMsg(msg, 4));
    // Flip a payload bit somewhere in the wire stream before delivery.
    assert(!mHal.txBuf.empty());
    mHal.txBuf[mHal.txBuf.size() / 2] ^= 0x01;
    pipe_data(mHal, sHal);

    uint8_t rx[32];
    int r = b.recvMsg(rx, sizeof(rx));
    // Either the per-frame CRC8 dropped the frame (nothing complete -> 0)
    // or the message CRC16 caught it (-1). Both are correct rejections.
    assert(r <= 0);
    assert(b.getErrCount() > 0);
    std::cout << "PASS" << std::endl;
}

void run_test_stats() {
    std::cout << "\n=== Test: Throughput Counters ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
    ALink a(mHal, true, cfg);
    ALink b(sHal, false, cfg);

    uint8_t msg[100];
    for (int i = 0; i < 100; i++) msg[i] = i;
    assert(a.sendMsg(msg, 100));
    pipe_data(mHal, sHal);
    uint8_t rx[128];
    assert(b.recvMsg(rx, sizeof(rx)) == 100);

    uint64_t atx, arx, btx, brx;
    a.getStats(atx, arx);
    b.getStats(btx, brx);
    assert(atx == 100 + MSG_HDR); // header + payload queued
    assert(brx == 100 + MSG_HDR); // header + payload delivered to app buffer

    a.resetStats();
    a.getStats(atx, arx);
    assert(atx == 0 && arx == 0);
    std::cout << "PASS" << std::endl;
}

void run_test_best_baud_selection() {
    std::cout << "\n=== Test: Best-Baud Picks Highest Working Index ===" << std::endl;
    // 4 bauds. Feed the slave 3 PINGs (it scores indices 0,1,2) then a REQ.
    // It must reply with index 2 (fastest baud that scored), not 3.
    MockHal sHal;
    AutoLinkConfig cfg; cfg.allowedBauds = {9600, 19200, 38400, 57600};
    ALink slave(sHal, false, cfg);
    slave.begin();
    assert(slave.getState() == State::SWP);

    auto frame = [&](uint8_t cmd, uint8_t* out) {
        out[0] = 0xAA; out[1] = 0x55; out[2] = cmd;
        uint8_t crc = 0;
        static const auto c8 = [](uint8_t crc, uint8_t d) {
            crc ^= d;
            for (int i = 0; i < 8; i++) crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1;
            return crc;
        };
        crc = c8(crc, out[0]); crc = c8(crc, out[1]); crc = c8(crc, out[2]);
        out[3] = crc;
    };

    uint8_t pf[4]; frame(PING_CMD, pf);
    slave.onRx(pf, 4); // scores[0]++, spdI->1
    slave.onRx(pf, 4); // scores[1]++, spdI->2
    slave.onRx(pf, 4); // scores[2]++, spdI->3
    assert(slave.getCurrentSpdIndex() == 3);

    uint8_t rf[4]; frame(REQ_CMD, rf);
    slave.onRx(rf, 4); // slave replies best index and goes OK
    assert(slave.getState() == State::OK);

    // Reply frame is {0xAA,0x55,best,crc}; best must be 2.
    assert(sHal.txBuf.size() == 4);
    assert(sHal.txBuf[2] == 2);
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
    run_test_message_roundtrip();
    run_test_message_boundaries_back_to_back();
    run_test_message_size_sweep();
    run_test_message_crc_reject();
    run_test_stats();
    run_test_best_baud_selection();
    std::cout << "\n=== All Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif // ARDUINO
