// UtilBlink pattern: start/flash/cancel.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <string>
#    include <vector>
#    include "al/util/UtilBlink.h"

using namespace autolink;

class MockBlinkHal : public IBlinkHal
{
public:
    std::vector<std::string> ev;
    void writePin(bool on) override { ev.push_back(on ? "HI" : "LO"); }
    void startOnce(uint32_t ms) override
    {
        ev.push_back("once" + std::to_string(ms));
    }
    void cancel() override { ev.push_back("cancel"); }
    void delayMs(uint32_t ms) override
    {
        ev.push_back("delay" + std::to_string(ms));
    }
    std::string joined() const
    {
        std::string s;
        for (auto &e : ev) {
            if (!s.empty())
                s += ",";
            s += e;
        }
        return s;
    }
};

void test_async_sequence()
{
    std::cout << "\n=== Test: Async Pattern Sequence ===" << std::endl;
    MockBlinkHal hal;
    UtilBlink b(hal);
    b.start(2, 60, 40);
    assert(b.active());
    b.tick();
    b.tick();
    b.tick();
    assert(!b.active());

    assert(hal.joined() == "cancel,LO,HI,once60,LO,once40,HI,once60,LO");

    size_t n = hal.ev.size();
    b.tick();
    assert(hal.ev.size() == n);
    std::cout << "PASS" << std::endl;
}

void test_async_single_flash()
{
    std::cout << "\n=== Test: Async Single Flash ===" << std::endl;
    MockBlinkHal hal;
    UtilBlink b(hal);
    b.start(1);
    b.tick();
    assert(!b.active());
    assert(hal.joined() == "cancel,LO,HI,once60,LO");
    std::cout << "PASS" << std::endl;
}

void test_restart_replaces()
{
    std::cout << "\n=== Test: Restart Replaces Running Pattern ==="
              << std::endl;
    MockBlinkHal hal;
    UtilBlink b(hal);
    b.start(5, 100, 100);
    hal.ev.clear();
    b.start(1, 30, 30);
    b.tick();
    assert(!b.active());
    assert(hal.joined() == "cancel,LO,HI,once30,LO");
    std::cout << "PASS" << std::endl;
}

void test_cancel()
{
    std::cout << "\n=== Test: Cancel Stops and Forces LED Off ===" << std::endl;
    MockBlinkHal hal;
    UtilBlink b(hal);
    b.start(3, 50, 50);
    assert(b.active());
    b.cancel();
    assert(!b.active());
    assert(hal.ev.back() == "LO");
    size_t n = hal.ev.size();
    b.tick();
    assert(hal.ev.size() == n);
    std::cout << "PASS" << std::endl;
}

void test_blocking_sequence()
{
    std::cout << "\n=== Test: Blocking Flash + Pause ===" << std::endl;
    MockBlinkHal hal;
    UtilBlink b(hal);
    b.flashBlocking(2, 100, 40, 2000);
    assert(hal.joined() ==
           "cancel,LO,HI,delay100,LO,delay40,HI,delay100,LO,delay2000");

    hal.ev.clear();
    b.flashBlocking(1, 60, 60, 0);
    assert(hal.joined() == "cancel,LO,HI,delay60,LO");
    std::cout << "PASS" << std::endl;
}

void test_blocking_cancels_async()
{
    std::cout << "\n=== Test: Blocking Call Cancels Async Pattern ==="
              << std::endl;
    MockBlinkHal hal;
    UtilBlink b(hal);
    b.start(10, 60, 60);
    b.flashBlocking(1, 50, 50, 100);
    assert(!b.active());
    b.tick();
    assert(hal.ev.back() == "delay100");
    std::cout << "PASS" << std::endl;
}

void test_invalid_n_ignored()
{
    std::cout << "\n=== Test: n <= 0 Is Ignored ===" << std::endl;
    MockBlinkHal hal;
    UtilBlink b(hal);
    b.start(0);
    b.start(-3);
    assert(hal.ev.empty());
    assert(!b.active());
    std::cout << "PASS" << std::endl;
}

int main()
{
    std::cout << "=== Running UtilBlink Tests ===" << std::endl;
    test_async_sequence();
    test_async_single_flash();
    test_restart_replaces();
    test_cancel();
    test_blocking_sequence();
    test_blocking_cancels_async();
    test_invalid_n_ignored();
    std::cout << "\n=== UtilBlink Tests Completed Successfully ==="
              << std::endl;
    return 0;
}

#endif