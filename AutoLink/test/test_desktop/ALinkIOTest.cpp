// ALinkIOTest.cpp — host-only tests for the ALink raw and reliable byte
// I/O path, plus a README-usage scenario. Arduino/ESP32 builds skip this.
#ifndef ARDUINO

#include <iostream>
#include <iomanip>
#include <chrono>
#include <cassert>
#include <vector>
#include "MockHal.h"

using namespace autolink;

void test_basic_io() {
    std::cout << "\n=== Test: Basic Write/Read/Peek/Flush/Available ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg; cfg.reliableMode = false; // raw byte path
    ALink ping(mHal, true, cfg);
    ALink pong(sHal, false, cfg);
    // Both nodes start in State::OK by constructor default. begin() is
    // deliberately not called here so this test exercises only the data
    // path in isolation -- mirrors a known-good-baud scenario.
    uint8_t data[] = {0x11, 0x22};
    ping.write(data, 2);
    ping.flush();

    pong.onRx(mHal.txBuf.data(), mHal.txBuf.size());

    assert(pong.available() == 2);
    assert(pong.peek() == 0x11);
    assert(pong.available() == 2);

    uint8_t rb_arr[10];
    assert(pong.read(rb_arr, 10) == 2);
    assert(rb_arr[0] == 0x11);
    assert(rb_arr[1] == 0x22);

    assert(pong.available() == 0);
    std::cout << "PASS" << std::endl;
}

void test_reliable_mode() {
    std::cout << "\n=== Test: Reliable Mode (COBS) ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg; cfg.reliableMode = true;
    ALink ping(mHal, true, cfg);
    ALink pong(sHal, false, cfg);

    uint8_t data[] = {0xAA, 0xBB};
    ping.write(data, 2);
    assert(!mHal.txBuf.empty());

    pong.onRx(mHal.txBuf.data(), mHal.txBuf.size());
    uint8_t rb_arr[10];
    assert(pong.read(rb_arr, 10) == 2);
    assert(rb_arr[0] == 0xAA);
    assert(rb_arr[1] == 0xBB);

    // Craft a valid COBS frame but with a wrong CRC byte so the receiver
    // calls err(). Payload {0x01,0x02} + bad CRC -> encoded {0x04,0x01,0x02,0xFF}.
    uint8_t bad_crc_frame[] = {0x00, 0x04, 0x01, 0x02, 0xFF, 0x00};
    pong.onRx(bad_crc_frame, sizeof(bad_crc_frame));
    assert(pong.getErrCount() > 0);

    std::cout << "PASS" << std::endl;
}

void test_throughput_and_sizes() {
    std::cout << "\n=== Test: Payloads & Throughput (Reliable Mode) ===" << std::endl;
    MockHal mHal, sHal;
    AutoLinkConfig cfg;
    cfg.reliableMode = true;
    cfg.streamBufferSize = 32000;
    ALink ping(mHal, true, cfg);
    ALink pong(sHal, false, cfg);

    std::vector<int> sizes = {0, 1, 2, 4, 8, 16, 32, 64, 128, 512, 1024, 2048, 4096, 8000, 16000};

    std::cout << std::left << std::setw(15) << "Payload Size"
              << std::setw(20) << "Time Taken (s)"
              << std::setw(20) << "Bytes/Sec" << std::endl;
    std::cout << std::string(55, '-') << std::endl;

    for (int sz : sizes) {
        std::vector<uint8_t> txData(sz > 0 ? sz : 1);
        std::vector<uint8_t> rxData(sz > 0 ? sz : 1);

        for (int i = 0; i < sz; i++) txData[i] = i & 0xFF;

        auto start = std::chrono::high_resolution_clock::now();

        if (sz > 0) ping.write(txData.data(), sz);

        pipe_data(mHal, sHal);

        int bytesRead = 0;
        if (sz > 0) {
            int chunk;
            while ((chunk = pong.read(rxData.data() + bytesRead, sz - bytesRead)) > 0) {
                bytesRead += chunk;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> diff = end - start;
        double bps = sz > 0 ? (sz / diff.count()) : 0.0;

        assert(bytesRead == sz);
        if (sz > 0) {
            for (int i = 0; i < sz; i++) {
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

void test_stats() {
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
    assert(atx == 100 + MSG_HDR);
    assert(brx == 100 + MSG_HDR);
    assert(aerr == 0);
    assert(berr == 0);

    a.resetStats();
    a.getStats(atx, arx, aerr);
    assert(atx == 0 && arx == 0);
    assert(aerr == 0);
    std::cout << "PASS" << std::endl;
}

void test_readme_usage() {
    std::cout << "\n=== Test: Real-world README Usage Simulation ===" << std::endl;

    AutoLinkConfig cfg;
    cfg.reliableMode = true;
    cfg.streamBufferSize = 2048;
    cfg.allowedBauds = {9600, 115200}; cfg.pingSamplesPerBaud = 1;

    MockHal txHal, rxHal;
    ALink txNode(txHal, true, cfg);
    ALink link(rxHal, false, cfg);

    txNode.begin();
    link.begin();

    txNode.onTimer(); pipe_data(txHal, rxHal); // SWP: PING@9600, spdI->1
    txNode.onTimer(); pipe_data(txHal, rxHal); // SWP: PING@115200, spdI->2 -> LCK
    txNode.onTimer(); pipe_data(txHal, rxHal); // LCK: REQ_CMD; pong -> OK, replies index
    pipe_data(rxHal, txHal);                   // ping receives baud index -> OK

    assert(txNode.getState() == State::OK);
    assert(link.getState()   == State::OK);

    uint8_t payload[] = {0xAB, 0xCD, 0xEF};
    txNode.write(payload, 3);
    pipe_data(txHal, rxHal);

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
    std::cout << "=== Running ALinkIO Tests ===" << std::endl;
    test_basic_io();
    test_reliable_mode();
    test_throughput_and_sizes();
    test_stats();
    test_readme_usage();
    std::cout << "\n=== ALinkIO Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif // ARDUINO
