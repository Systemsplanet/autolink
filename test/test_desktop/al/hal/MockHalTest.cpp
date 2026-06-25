// MockHal: pipe wiring, clock, drop model.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include "MockHal.h"

using namespace autolink;

void test_set_spd_records_history()
{
    std::cout << "\n=== Test: MockHal setSpd Records History ===" << std::endl;
    MockHal hal;
    hal.setSpd(115200);
    hal.setSpd(9600);
    assert(hal.spd == 9600);
    assert(hal.spdHistory.size() == 2);
    assert(hal.spdHistory[0] == 115200);
    assert(hal.spdHistory[1] == 9600);
    std::cout << "PASS" << std::endl;
}

void test_send_break_self_delivery_is_noop_without_peer()
{
    std::cout << "\n=== Test: sendBreak Without Peer Doesn't Self-Deliver ==="
              << std::endl;
    MockHal hal;
    hal.bind(new Link(hal, true, AutoLinkConfig()));
    hal.sendBreak();
    assert(hal.sendBreakCalls == 1);

    assert(hal.link->getState() == State::OK);
    delete hal.link;
    hal.link = nullptr;
    std::cout << "PASS" << std::endl;
}

void test_send_break_delivers_to_peer()
{
    std::cout << "\n=== Test: sendBreak Delivers to Peer MockHal ==="
              << std::endl;
    MockHal a, b;
    a.peer = &b;
    a.bind(new Link(a, true, AutoLinkConfig()));
    b.bind(new Link(b, false, AutoLinkConfig()));

    assert(a.link->getState() == State::OK);
    assert(b.link->getState() == State::OK);
    a.sendBreak();
    assert(a.sendBreakCalls == 1);
    assert(b.link->getState() == State::SWP);
    assert(a.link->getState() == State::OK);
    delete a.link;
    a.link = nullptr;
    delete b.link;
    b.link = nullptr;
    std::cout << "PASS" << std::endl;
}

void test_tx_buffer_accumulates()
{
    std::cout << "\n=== Test: MockHal tx Accumulates Buffer ===" << std::endl;
    MockHal hal;
    uint8_t a = 0xFF;
    hal.tx(&a, 1);
    uint8_t b_arr[] = { 0x01, 0x02, 0x03 };
    hal.tx(b_arr, 3);
    assert(hal.txBuf.size() == 4);
    assert(hal.txBuf[0] == 0xFF);
    assert(hal.txBuf[1] == 0x01);
    assert(hal.txBuf[3] == 0x03);
    hal.clearTx();
    assert(hal.txBuf.empty());
    std::cout << "PASS" << std::endl;
}

void test_timer_state_transitions()
{
    std::cout << "\n=== Test: MockHal startTimer / stopTimer ===" << std::endl;
    MockHal hal;
    hal.startTimer(50);
    assert(hal.timerActive);
    assert(hal.lastTimerMs == 50);
    hal.stopTimer();
    assert(!hal.timerActive);
    std::cout << "PASS" << std::endl;
}

void test_lock_unlock_balanced()
{
    std::cout << "\n=== Test: MockHal lock / unlock Are Balanced ==="
              << std::endl;
    MockHal hal;
    hal.lock();
    hal.unlock();

    hal.lock();
    hal.unlock();
    std::cout << "PASS" << std::endl;
}

void test_app_buffer_push_peek_pop_clear()
{
    std::cout << "\n=== Test: MockHal App Buffer Push/Peek/Pop/Clear ==="
              << std::endl;
    MockHal hal;
    uint8_t pb[] = { 0xAA, 0xBB };
    hal.pushAppBuf(pb, 2);
    assert(hal.appBufAvailable() == 2);
    assert(hal.peekAppBuf() == 0xAA);
    assert(hal.appBufAvailable() == 2);

    uint8_t rb[2];
    assert(hal.popAppBuf(rb, 2) == 2);
    assert(rb[0] == 0xAA && rb[1] == 0xBB);
    assert(hal.appBufAvailable() == 0);
    {
        uint8_t b = 0;
        assert(hal.popAppBuf(&b, 1) == 0);
    }

    uint8_t one = 0xCC;
    hal.pushAppBuf(&one, 1);
    hal.clearAppBuf();
    assert(hal.appBufAvailable() == 0);
    std::cout << "PASS" << std::endl;
}

void test_app_buffer_push_respects_capacity()
{
    std::cout << "\n=== Test: MockHal App Buffer Capacity Cap ===" << std::endl;
    MockHal hal;
    hal.appBufCap = 4;
    uint8_t big[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int n = hal.pushAppBuf(big, 8);
    assert(n == 4);
    assert(hal.appBufAvailable() == 4);
    std::cout << "PASS" << std::endl;
}

void test_now_ms_is_injectable()
{
    std::cout << "\n=== Test: MockHal nowMs Is Injectable ===" << std::endl;
    MockHal hal;
    hal.now = 0;
    assert(hal.nowMs() == 0);
    hal.now = 0xDEADBEEF;
    assert(hal.nowMs() == 0xDEADBEEF);
    std::cout << "PASS" << std::endl;
}

int main()
{
    std::cout << "=== Running MockHal Tests ===" << std::endl;
    test_set_spd_records_history();
    test_send_break_self_delivery_is_noop_without_peer();
    test_send_break_delivers_to_peer();
    test_tx_buffer_accumulates();
    test_timer_state_transitions();
    test_lock_unlock_balanced();
    test_app_buffer_push_peek_pop_clear();
    test_app_buffer_push_respects_capacity();
    test_now_ms_is_injectable();
    std::cout << "\n=== MockHal Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif