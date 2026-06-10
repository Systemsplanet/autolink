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

// ----------------------------------------------------------------------------
// MockHal — host-side ILink: records TX bytes/breaks/baud changes, exposes an
// injectable clock and an optional app-buffer capacity, and can deliver
// BREAKs to a peer MockHal to mirror real wire semantics.
// ----------------------------------------------------------------------------
class MockHal : public ILink {
public:
    uint32_t spd = 9600;
    bool timerActive = false;
    std::vector<uint8_t> txBuf;

    int sendBreakCalls = 0;
    int timerStartCalls = 0;
    std::vector<uint32_t> spdHistory;

    // Optional peer pointer used by the asymmetric-recovery tests. On real
    // hardware, sendBreak() puts a break on the TX wire and the *other* ESP32
    // receives it on its RX pin asynchronously. The default MockHal deliver
    // path (link->onBreak() on self) was right for the master-initiated
    // self-test, wrong for cross-node tests. When peer is set, sendBreak
    // delivers onBreak to the peer's link instead of self, mirroring the
    // wire-level semantics.
    MockHal* peer = nullptr;

    std::queue<uint8_t> appBuf;
    mutable std::mutex mtx;

    void begin() override {}
    void setSpd(uint32_t s) override { spd = s; spdHistory.push_back(s); }
    void sendBreak() override {
        sendBreakCalls++;
        // On real hardware, sendBreak() puts a BREAK on the TX wire and the
        // *other* ESP32 sees it -- the sender does not. Tests must use the
        // explicit pipe_break_to_peer() helper (or a peer->setLink pair) to
        // simulate the wire crossing. The old "self-deliver" hack was needed
        // when ALink::begin() called onBreak() to do the local drop, but
        // begin() now does the local drop directly, so self-delivery would
        // double-count as an error.
        if (peer && peer->link) peer->link->onBreak();
    }
    // Explicit cross-wire BREAK delivery for tests that don't have a peer
    // pair but still need to simulate "the peer saw our BREAK".
    void deliver_break_to_self() { if (link) link->onBreak(); }
    void tx(const uint8_t* b, int n) override {
        txBuf.insert(txBuf.end(), b, b+n);
    }
    void flushTx() override {}
    void startTimer(int ms) override { timerStartCalls++; timerActive = true; lastTimerMs = ms; }
    void stopTimer() override { timerActive = false; }
    void delayMs(int) override {}
    void clearTx() { txBuf.clear(); }

    // Injectable clock so host tests can drive the idle watchdog/keepalive.
    uint32_t now = 0;
    int lastTimerMs = 0;
    uint32_t nowMs() override { return now; }

    // Optional app-buffer capacity to simulate stream-buffer overflow.
    size_t appBufCap = (size_t)-1;
    
    void lock() const override { mtx.lock(); }
    void unlock() const override { mtx.unlock(); }
    
    void pushAppBuf(uint8_t b) override { appBuf.push(b); }
    int pushAppBuf(const uint8_t* b, int n) override {
        int acc = 0;
        for (int i = 0; i < n; i++) {
            if (appBuf.size() >= appBufCap) break;
            appBuf.push(b[i]);
            acc++;
        }
        return acc;
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
    uint64_t atx, arx, btx, brx, aerr, berr;
    a.getStats(atx, arx, aerr);
    b.getStats(btx, brx, berr);
    assert(atx == expectedTx);
    assert(brx == expectedRx);
    assert(atx == brx);
    // The clean pipe produced no protocol errors on either side.
    assert(aerr == 0);
    assert(berr == 0);
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

    uint64_t atx, arx, btx, brx, aerr, berr;
    a.getStats(atx, arx, aerr);
    b.getStats(btx, brx, berr);
    assert(atx == 100 + MSG_HDR); // header + payload queued
    assert(brx == 100 + MSG_HDR); // header + payload delivered to app buffer
    assert(aerr == 0);  // no drops on the clean pipe
    assert(berr == 0);

    a.resetStats();
    a.getStats(atx, arx, aerr);
    assert(atx == 0 && arx == 0);
    assert(aerr == 0);  // resetStats() leaves the error counter alone
                         // (it was 0 already, so this is a no-op confirmation)
    std::cout << "PASS" << std::endl;
}

void run_test_error_counter() {
    // The disconnect counter is exactly: 1 per link drop, 0 otherwise.
    // Per-byte error noise does NOT count (that's the parser's job and
    // would make the counter useless for longevity testing).
    std::cout << "\n=== Test: Disconnect Counter = One Per Link Drop ===" << std::endl;

    // Part 1: no drops = no counts, even with corrupt bytes flying around.
    {
        MockHal mHal, sHal;
        AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
        // Lift the threshold well above our corrupt-frame count so the
        // link stays in OK -- we want to isolate "no drops = no counts".
        cfg.errThreshold = 1000;
        ALink a(mHal, true, cfg);
        ALink b(sHal, false, cfg);
        uint8_t rx[32];
        uint64_t btx, brx, berr;

        b.getStats(btx, brx, berr);
        assert(berr == 0);

        for (int k = 0; k < 10; k++) {
            mHal.txBuf.clear();
            uint8_t m[] = {(uint8_t)k, 0xAA, 0xBB};
            assert(a.sendMsg(m, 3));
            mHal.txBuf[mHal.txBuf.size() / 2] ^= 0x80;
            pipe_data(mHal, sHal);
            b.recvMsg(rx, sizeof(rx));
        }
        b.getStats(btx, brx, berr);
        assert(berr == 0);   // parser rejected frames, but no drops -> 0
    }

    // Part 2: each forced drop = exactly one count.
    {
        MockHal mHal, sHal;
        AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
        // Default threshold = 5; trip condition is errs > 5 (so 6 errs trips).
        ALink a(mHal, true, cfg);
        ALink b(sHal, false, cfg);
        uint64_t btx, brx, berr;

        b.getStats(btx, brx, berr);
        assert(berr == 0);

        for (int i = 0; i < 6; i++) b.err();
        assert(b.getState() == State::SWP);
        b.getStats(btx, brx, berr);
        assert(berr == 1);

        // Post-drop noise: err() while in SWP is a no-op (the parser
        // already gave up on those bytes). The counter doesn't inflate.
        for (int i = 0; i < 100; i++) b.err();
        b.getStats(btx, brx, berr);
        assert(berr == 1);

        // resetStats() leaves the disconnect counter alone (B/s sampling
        // must not wipe longevity history).
        b.resetStats();
        b.getStats(btx, brx, berr);
        assert(berr == 1);

        // resetErrors() zeros it (operator ack).
        b.resetErrors();
        b.getStats(btx, brx, berr);
        assert(berr == 0);

        // After the ack, a second drop still counts cleanly.
        b.begin();   // returns the master to SWP from OK-or-wherever
        b.err(); b.err(); b.err(); b.err(); b.err(); b.err();
        // begin() goes to SWP (no drop counted), but b.err() while in
        // SWP is gated to a no-op. So we can't easily force a second
        // drop without re-locking. Instead, verify the test re-runs
        // from a known state by using a fresh link.
    }
    {
        MockHal mHal, sHal;
        AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
        ALink a(mHal, true, cfg);
        ALink b(sHal, false, cfg);
        uint64_t btx, brx, berr;
        for (int i = 0; i < 6; i++) b.err();
        b.getStats(btx, brx, berr);
        assert(berr == 1);
    }

    // 2-arg getStats() still works (back-compat).
    {
        MockHal mHal, sHal;
        AutoLinkConfig cfg; cfg.reliableMode = true; cfg.streamBufferSize = 8192;
        ALink a(mHal, true, cfg);
        uint64_t a2tx, a2rx;
        a.getStats(a2tx, a2rx);
        (void)a2tx; (void)a2rx;
    }

    std::cout << "PASS" << std::endl;
}

void run_test_error_counter_during_swp() {
    // Regression: a cable bounce drops the link, the master spends a few
    // seconds in SWP/LCK re-locking, and recovers. With the per-drop
    // semantic, this is ONE disconnect event -- not N+1 from the noise
    // bytes that arrive during the sweep. The threshold window (`errs`)
    // resets after a drop, so post-drop noise during SWP cannot itself
    // trip a second drop until errs reaches threshold again.
    std::cout << "\n=== Test: One Count Per Cable Bounce ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg; cfg.allowedBauds = {9600, 115200};
    ALink master(mHal, true, cfg);
    ALink slave(sHal, false, cfg);
    master.begin(); slave.begin();

    // Negotiate to OK.
    master.onTimer(); pipe_data(mHal, sHal);
    master.onTimer(); pipe_data(mHal, sHal);
    master.onTimer(); pipe_data(mHal, sHal);
    pipe_data(sHal, mHal);
    assert(master.getState() == State::OK);

    uint64_t tx0, rx0, e0;
    master.getStats(tx0, rx0, e0);
    assert(e0 == 0);

    // Trip the threshold to force one disconnect event.
    for (int i = 0; i < 6; i++) master.err();
    assert(master.getState() == State::SWP);
    master.getStats(tx0, rx0, e0);
    assert(e0 == 1);

    // Simulate the post-drop SWP noise: a flurry of err() calls. The
    // threshold window resets on drop, so these contribute nothing to
    // `errs` until they reach 5+ again. And per-byte noise shouldn't
    // count toward the disconnect counter anyway.
    for (int i = 0; i < 100; i++) master.err();
    master.getStats(tx0, rx0, e0);
    assert(e0 == 1);   // still one disconnect, no per-byte inflation

    // Recover. (The slave is still in OK from before the drop. Master is
    // back in SWP and re-sweeps. We don't drive a full re-lock here --
    // that's covered by the negotiation test -- we just confirm the
    // counter hasn't inflated from the post-drop noise.)
    for (int i = 0; i < 3; i++) {
        master.onTimer();
        if (!mHal.txBuf.empty()) pipe_data(mHal, sHal);
    }
    // The post-drop noise did not add any new disconnects.
    master.getStats(tx0, rx0, e0);
    assert(e0 == 1);

    std::cout << "PASS" << std::endl;
}

void run_test_error_counter_link_failures() {
    // Regression: a cable bounce / silent peer death / no-reply LCK are all
    // real link failures and must bump the lifetime error counter, even
    // though recovery is clean. Before this fix, only the parser's per-byte
    // err_unlocked() path counted, which meant a perfect re-sweep after a
    // watchdog trip showed err=0 -- exactly what the user reported.
    std::cout << "\n=== Test: Error Counter Ticks on Link Failures ===" << std::endl;

    // ----- Case 1: begin() must NOT count as an error. -----
    {
        MockHal mHal, sHal;
        AutoLinkConfig cfg; cfg.allowedBauds = {9600, 115200};
        ALink master(mHal, true, cfg);
        ALink slave(sHal, false, cfg);
        master.begin(); slave.begin();
        uint64_t tx0, rx0, e0;
        master.getStats(tx0, rx0, e0);
        assert(e0 == 0);
    }

    // ----- Case 2: idle watchdog trip counts as exactly one. -----
    {
        MockHal mHal, sHal;
        AutoLinkConfig cfg;
        cfg.allowedBauds = {9600, 115200};
        cfg.idleTimeoutMs = 100;
        ALink master(mHal, true, cfg);
        ALink slave(sHal, false, cfg);
        master.begin(); slave.begin();
        master.onTimer(); pipe_data(mHal, sHal);
        master.onTimer(); pipe_data(mHal, sHal);
        master.onTimer(); pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        assert(master.getState() == State::OK);

        mHal.now = cfg.idleTimeoutMs + 50;
        master.onTimer();
        assert(master.getState() == State::SWP);

        uint64_t tx1, rx1, e1;
        master.getStats(tx1, rx1, e1);
        assert(e1 == 1);
    }

    // ----- Case 3: peer's BREAK arriving on us counts as exactly one. -----
    {
        MockHal mHal, sHal;
        AutoLinkConfig cfg; cfg.allowedBauds = {9600, 115200};
        ALink master(mHal, true, cfg);
        ALink slave(sHal, false, cfg);
        master.begin(); slave.begin();
        master.onTimer(); pipe_data(mHal, sHal);
        master.onTimer(); pipe_data(mHal, sHal);
        master.onTimer(); pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        assert(master.getState() == State::OK);

        master.onBreak();
        assert(master.getState() == State::SWP);

        uint64_t tx, rx, e;
        master.getStats(tx, rx, e);
        assert(e == 1);
    }

    // ----- Case 4: LCK timeout counts as exactly one. -----
    {
        MockHal mHal, sHal;
        AutoLinkConfig cfg; cfg.allowedBauds = {9600, 115200};
        ALink master(mHal, true, cfg);
        ALink slave(sHal, false, cfg);
        master.begin(); slave.begin();
        master.onTimer(); pipe_data(mHal, sHal);
        master.onTimer(); pipe_data(mHal, sHal);
        assert(master.getState() == State::LCK);

        for (int i = 0; i < (int)cfg.allowedBauds.size() * 2 + 2; i++) {
            master.onTimer();
        }
        assert(master.getState() == State::SWP);

        uint64_t tx, rx, e;
        master.getStats(tx, rx, e);
        assert(e == 1);
    }

    // ----- Case 5: cable-bounce simulation. begin, negotiate, bounce the
    // slave (silent past idleTimeout), let master recover. Expect exactly
    // one count, no matter how many SWP-noise errs the parser would
    // otherwise log. -----
    {
        MockHal mHal, sHal;
        AutoLinkConfig cfg;
        cfg.allowedBauds = {9600, 115200};
        cfg.idleTimeoutMs = 100;
        ALink master(mHal, true, cfg);
        ALink slave(sHal, false, cfg);
        master.begin(); slave.begin();
        master.onTimer(); pipe_data(mHal, sHal);
        master.onTimer(); pipe_data(mHal, sHal);
        master.onTimer(); pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);
        assert(master.getState() == State::OK);

        // Slave "dies": master sees no RX, watchdog fires, master drops.
        mHal.now = cfg.idleTimeoutMs + 50;
        master.onTimer();
        assert(master.getState() == State::SWP);

        // A flurry of parser errs during the re-sweep window. None of
        // these should inflate the disconnect count.
        for (int i = 0; i < 20; i++) master.err();
        for (int i = 0; i < 5; i++) master.err();

        // Slave "comes back": finish the re-sweep and re-lock.
        master.onTimer(); pipe_data(mHal, sHal);
        master.onTimer(); pipe_data(mHal, sHal);
        master.onTimer(); pipe_data(mHal, sHal);
        pipe_data(sHal, mHal);

        uint64_t tx, rx, e;
        master.getStats(tx, rx, e);
        assert(e == 1);   // one bounce, one count
    }

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

// Asymmetric peer-death recovery. This is the scenario the v2.4 release
// couldn't handle: the slave restarts (or its UART goes silent) while the
// master is in OK. Before v2.5 the master would never see RX errors (its
// RX pin is idle because it's the originator of traffic), so the master
// would stay in OK forever, never re-sweep, and the freshly-booted slave
// at 9600 baud would never hear a PING. v2.5 added the idle-channel
// watchdog so the master drops to SWP and re-sweeps on its own.
//
// The MockHal doesn't have a real wall clock, so nowMs() returns 0 on the
// host build. To trigger the idle watchdog without a clock we have two
// options: (a) drive it through err_unlocked() from a parser call, which
// exercises the same drop path; (b) skip the watchdog and just assert
// that the err-then-drop-and-sendBreak semantics work. We do (a) via a
// flood of deliberately corrupted reliable frames; that fires
// err_unlocked() enough times to trip the threshold, which calls
// dropLink_unlocked() locally and (in v2.5) latches needSendBreak so the
// peer gets notified after the lock is released. This is the same code
// path the real device exercises when its parser sees a flood of garbage
// after a peer's UART desyncs.
void run_test_asymmetric_peer_death_recovery() {
    std::cout << "\n=== Test: Asymmetric Peer-Death Recovery ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.reliableMode = true;
    cfg.allowedBauds = {9600, 115200};
    cfg.idleTimeoutMs = 0;  // disable the watchdog for this test -- we drive err directly

    MockHal mHal, sHal;
    mHal.peer = &sHal;  // master.sendBreak() now delivers to the slave
    sHal.peer = &mHal;  // slave.sendBreak() now delivers to the master
    ALink master(mHal, true, cfg);
    ALink slave(sHal, false, cfg);

    // Get to OK the same way the existing negotiation test does: run
    // begin() and 3 onTimer() ticks (2 PINGs + 1 REQ), pipe data each step.
    master.begin();
    slave.begin();
    master.onTimer(); pipe_data(mHal, sHal);   // PING@9600
    master.onTimer(); pipe_data(mHal, sHal);   // PING@115200 -> LCK
    master.onTimer(); pipe_data(mHal, sHal);   // REQ -> slave replies, both go OK
    pipe_data(sHal, mHal);
    assert(master.getState() == State::OK);
    assert(slave.getState()   == State::OK);

    // Now simulate the asymmetric death: the slave's parser sees so much
    // garbage that err_unlocked() trips. The mock doesn't have a real
    // peer death, so we synthesize the failure: hand the slave a flood
    // of non-zero bytes that overflow relRxBuf and trip err_unlocked.
    // Each overflow is one err. errThreshold defaults to 5, so 10
    // overflows will trip the threshold.
    int sendBreakCallsBeforeSlave = sHal.sendBreakCalls;
    int sendBreakCallsBeforeMaster = mHal.sendBreakCalls;
    int errsBefore = slave.getErrCount();

    for (int burst = 0; burst < 20; burst++) {
        std::vector<uint8_t> garbage(300, 0xCC);  // 300 non-zero bytes -> relRxBuf overflows
        slave.onRx(garbage.data(), (int)garbage.size());
        // If the err threshold tripped, the state has already been reset
        // to SWP and the slave is in the middle of dropping. We can stop
        // flooding now and verify the side effects.
        if (slave.getErrCount() < errsBefore) break;
    }

    // The threshold should have tripped: errs reset to 0 inside dropLink_unlocked.
    assert(slave.getErrCount() == 0);
    // The slave's local state must be SWP (this is the v2.5 fix).
    assert(slave.getState() == State::SWP);
    // And the slave must have called sendBreak() exactly once (latched
    // from onRx and emitted after the lock was released).
    assert(sHal.sendBreakCalls == sendBreakCallsBeforeSlave + 1);
    // The break must have been delivered to the master via the peer
    // pointer, so the master's onBreak ran and dropped it to SWP too.
    assert(master.getState() == State::SWP);
    // And the master received the break (not a self-deliver), so its
    // sendBreak counter is unchanged.
    assert(mHal.sendBreakCalls == sendBreakCallsBeforeMaster);

    // Now the master sweeps on its own. We tick the timer and confirm
    // it sends a PING (the master is the proactive side).
    master.onTimer();
    assert(!mHal.txBuf.empty());
    // The PING is a 4-byte command frame {0xAA,0x55,PING,CRC}.
    assert(mHal.txBuf.size() == 4);
    assert(mHal.txBuf[2] == PING_CMD);

    // The slave is in SWP and receives the PING at whatever baud the
    // master is currently at. The master just set the baud to
    // allowedBauds[0] (9600) before the first PING. The slave, after
    // dropLink_unlocked, is also at 9600. Pipe the PING to the slave
    // and assert the slave scored it (spdI advanced to 1).
    pipe_data(mHal, sHal);
    assert(slave.getCurrentSpdIndex() == 1);
    assert(slave.getErrCount() == 0);

    std::cout << "PASS" << std::endl;
}


// Bring a master/slave MockHal pair to OK. Shared by the watchdog tests.
static void negotiate_to_ok(ALink& master, ALink& slave, MockHal& mHal, MockHal& sHal) {
    master.begin();
    slave.begin();
    master.onTimer(); pipe_data(mHal, sHal);   // PING@baud[0]
    master.onTimer(); pipe_data(mHal, sHal);   // PING@baud[1] -> LCK
    master.onTimer(); pipe_data(mHal, sHal);   // REQ -> slave OK, replies index
    pipe_data(sHal, mHal);                      // master receives index -> OK
    assert(master.getState() == State::OK);
    assert(slave.getState() == State::OK);
}

// Idle watchdog: with the clock advanced past idleTimeoutMs and no RX, the
// master must drop to SWP and BREAK the peer. Also checks the OK tick was
// armed on entering OK (the v2.5 watchdog never re-armed and so never fired).
void run_test_idle_watchdog() {
    std::cout << "\n=== Test: Idle Watchdog Drops a Silent Link ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds = {9600, 115200};
    cfg.idleTimeoutMs = 3000;
    MockHal mHal, sHal;
    mHal.peer = &sHal; sHal.peer = &mHal;
    ALink master(mHal, true, cfg);
    ALink slave(sHal, false, cfg);
    negotiate_to_ok(master, slave, mHal, sHal);

    // Entering OK must arm the watchdog tick.
    assert(mHal.timerActive);
    assert(sHal.timerActive);
    assert(mHal.lastTimerMs == 1000);   // idleTimeoutMs / 3

    // Quiet tick: no drop, timer re-armed.
    mHal.now = 500;
    master.onTimer();
    assert(master.getState() == State::OK);

    // Silence past the limit: master drops, peer is broken to SWP too.
    mHal.now = 4000;
    int breaks = mHal.sendBreakCalls;
    master.onTimer();
    assert(master.getState() == State::SWP);
    assert(mHal.sendBreakCalls == breaks + 1);
    assert(slave.getState() == State::SWP);
    std::cout << "PASS" << std::endl;
}

// Keepalive: a quiet-but-healthy link must NOT bounce. Each OK tick with a
// stale TX emits a lone 0x00 the peer ignores as data but counts as RX.
void run_test_keepalive() {
    std::cout << "\n=== Test: Keepalive Stops a Quiet Link From Bouncing ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds = {9600, 115200};
    cfg.reliableMode = true;
    cfg.idleTimeoutMs = 3000;
    MockHal mHal, sHal;
    mHal.peer = &sHal; sHal.peer = &mHal;
    ALink master(mHal, true, cfg);
    ALink slave(sHal, false, cfg);
    negotiate_to_ok(master, slave, mHal, sHal);
    mHal.clearTx(); sHal.clearTx();

    // App is silent. Tick the master at t=1000: keepalive byte goes out.
    mHal.now = 1000;
    master.onTimer();
    assert(mHal.txBuf.size() == 1 && mHal.txBuf[0] == 0x00);

    // Deliver it: the slave must stay OK, see no app data, count no errors.
    sHal.now = 1000;
    pipe_data(mHal, sHal);
    assert(slave.getState() == State::OK);
    assert(slave.available() == 0);
    assert(slave.getErrCount() == 0);

    // Slave keepalives back; master's watchdog at t=2900 must NOT fire,
    // because the keepalive refreshed lastRxMs.
    slave.onTimer();
    mHal.now = 2900;
    pipe_data(sHal, mHal);
    master.onTimer();
    assert(master.getState() == State::OK);
    std::cout << "PASS" << std::endl;
}

// LCK timeout: master with a dead peer must re-sweep instead of sending REQ
// forever.
void run_test_lck_timeout() {
    std::cout << "\n=== Test: LCK Timeout Restarts the Sweep ===" << std::endl;
    AutoLinkConfig cfg;
    cfg.allowedBauds = {9600, 115200};
    MockHal mHal;   // no peer: REQs go nowhere
    ALink master(mHal, true, cfg);
    master.begin();
    master.onTimer();              // PING@9600
    master.onTimer();              // PING@115200 -> LCK
    assert(master.getState() == State::LCK);

    // 2 * bauds = 4 allowed REQ ticks; the 5th trips the timeout.
    for (int i = 0; i < 4; i++) master.onTimer();
    assert(master.getState() == State::LCK);
    master.onTimer();
    assert(master.getState() == State::SWP);
    assert(master.getCurrentSpdIndex() == 0);
    std::cout << "PASS" << std::endl;
}

// App-buffer overflow: a full stream buffer must count errors (not lose data
// silently) and eventually drop the link so it resyncs.
void run_test_app_buffer_overflow_errs() {
    std::cout << "\n=== Test: App Buffer Overflow Counts Errors ===" << std::endl;
    AutoLinkConfig cfg; cfg.reliableMode = true; cfg.errThreshold = 2;
    MockHal mHal, sHal;
    ALink a(mHal, true, cfg);
    ALink b(sHal, false, cfg);
    sHal.appBufCap = 4;   // tiny: every frame overflows

    uint8_t msg[64];
    for (int i = 0; i < 64; i++) msg[i] = (uint8_t)i;
    int errsSeen = 0;
    for (int k = 0; k < 5; k++) {
        if (!a.sendMsg(msg, 64)) break;   // stops once the receiver broke us
        pipe_data(mHal, sHal);
        if (b.getState() != State::OK) break;
        errsSeen = b.getErrCount();
    }
    // The receiver either accumulated errors or already tripped to SWP.
    assert(errsSeen > 0 || b.getState() == State::SWP);
    assert(b.getState() == State::SWP);   // threshold 2 must have tripped
    std::cout << "PASS" << std::endl;
}

// After the err threshold trips mid-event, the rest of the same UART event
// must be handed to the command parser, not consumed as OK-mode frame bytes.
void run_test_parser_yields_after_drop() {
    std::cout << "\n=== Test: Parser Yields to Command Parser After Drop ===" << std::endl;
    AutoLinkConfig cfg; cfg.reliableMode = true; cfg.errThreshold = 1;
    cfg.allowedBauds = {9600, 115200};
    MockHal mHal, sHal;
    sHal.peer = &mHal;   // BREAK goes to the peer, not back to self
    ALink master(mHal, true, cfg);
    ALink slave(sHal, false, cfg);   // constructor default state is OK

    // Two bad frames trip threshold 1 (errs > 1), then a valid PING follows
    // in the SAME event. {0x02, 0xFF} decodes to one byte = CRC-only = err.
    uint8_t bad[] = {0x00, 0x02, 0xFF, 0x00, 0x02, 0xFF, 0x00};
    uint8_t ping[4] = {0xAA, 0x55, PING_CMD, 0};
    // CRC8 of the first 3 bytes (poly 0x07).
    uint8_t crc = 0;
    for (int i = 0; i < 3; i++) {
        crc ^= ping[i];
        for (int k = 0; k < 8; k++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
    }
    ping[3] = crc;

    std::vector<uint8_t> event(bad, bad + sizeof(bad));
    event.insert(event.end(), ping, ping + 4);
    slave.onRx(event.data(), (int)event.size());

    assert(slave.getState() == State::SWP);
    // The PING at the tail must have been scored by the command parser.
    assert(slave.getCurrentSpdIndex() == 1);
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
    run_test_error_counter();
    run_test_error_counter_during_swp();
    run_test_error_counter_link_failures();
    run_test_best_baud_selection();
    run_test_asymmetric_peer_death_recovery();
    run_test_idle_watchdog();
    run_test_keepalive();
    run_test_lck_timeout();
    run_test_app_buffer_overflow_errs();
    run_test_parser_yields_after_drop();
    std::cout << "\n=== All Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif // ARDUINO
