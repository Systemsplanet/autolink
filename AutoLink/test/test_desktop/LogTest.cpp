// Host-only unit tests for Log. Arduino/ESP32 builds skip this file.
#ifndef ARDUINO

#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <cstdio>
#include "Log.h"

using namespace autolink;

// Each emit() also fires the registered sink. Capture every line and assert
// on the (sev, tag, msg) triple. The sink must run after the normal output
// so we can verify ordering by reading stdout and the capture together.
struct CapturedLine {
    char     sev;
    std::string tag;
    std::string msg;
};
static std::vector<CapturedLine> g_captured;
static void captureSink(char sev, const char* tag, const char* msg, void*) {
    g_captured.push_back({sev, tag ? tag : "", msg ? msg : ""});
}

void test_level_filtering() {
    std::cout << "\n=== Test: Log Level Filtering ===" << std::endl;
    Log& L = Log::getLog();
    L.setSink(captureSink);
    g_captured.clear();

    L.setLevel(Log::ERROR);
    L.error("T", "err1");
    L.info ("T", "info1");
    L.debug("T", "dbg1");
    assert(g_captured.size() == 1);
    assert(g_captured[0].sev == 'E' && g_captured[0].msg == "err1");

    L.setLevel(Log::INFO);
    L.error("T", "err2");
    L.info ("T", "info2");
    L.debug("T", "dbg2");
    assert(g_captured.size() == 3);
    assert(g_captured[1].sev == 'E' && g_captured[1].msg == "err2");
    assert(g_captured[2].sev == 'I' && g_captured[2].msg == "info2");

    L.setLevel(Log::DEBUG);
    L.error("T", "err3");
    L.info ("T", "info3");
    L.debug("T", "dbg3");
    assert(g_captured.size() == 6);
    assert(g_captured[3].sev == 'E' && g_captured[3].msg == "err3");
    assert(g_captured[4].sev == 'I' && g_captured[4].msg == "info3");
    assert(g_captured[5].sev == 'D' && g_captured[5].msg == "dbg3");
    std::cout << "PASS" << std::endl;
}

void test_sink_registration_and_clearing() {
    std::cout << "\n=== Test: Sink Registration & Clearing ===" << std::endl;
    Log& L = Log::getLog();
    L.setLevel(Log::DEBUG);
    L.setSink(captureSink);
    g_captured.clear();
    L.info("TagA", "hello %d", 42);
    assert(g_captured.size() == 1);
    assert(g_captured[0].sev == 'I');
    assert(g_captured[0].tag == "TagA");
    assert(g_captured[0].msg == "hello 42");

    L.clearSink();
    g_captured.clear();
    L.info("TagA", "should be lost");
    assert(g_captured.empty());
    std::cout << "PASS" << std::endl;
}

void test_sink_context_pointer_passes_through() {
    std::cout << "\n=== Test: Sink Context Pointer ===" << std::endl;
    Log& L = Log::getLog();
    L.setLevel(Log::DEBUG);
    int marker = 0xCAFE;
    auto fn = [](char, const char*, const char*, void* ctx) {
        int* m = reinterpret_cast<int*>(ctx);
        *m = 0xBEEF;
    };
    L.setSink(fn, &marker);
    L.info("T", "x");
    assert(marker == 0xBEEF);
    L.clearSink();
    std::cout << "PASS" << std::endl;
}

void test_sink_called_within_emit() {
    std::cout << "\n=== Test: Sink Called Within emit() ===" << std::endl;
    Log& L = Log::getLog();
    L.setLevel(Log::DEBUG);
    L.setSink(captureSink);
    g_captured.clear();
    L.error("X", "emit order");
    L.info ("X", "second line");
    assert(g_captured.size() == 2);
    assert(g_captured[0].msg == "emit order");
    assert(g_captured[1].msg == "second line");
    L.clearSink();
    std::cout << "PASS" << std::endl;
}

void test_long_message_truncated_at_buffer() {
    std::cout << "\n=== Test: Long Message Truncated ===" << std::endl;
    Log& L = Log::getLog();
    L.setLevel(Log::DEBUG);
    L.setSink(captureSink);
    g_captured.clear();
    // 1024 chars > 256-byte internal msg buffer; vsnprintf truncates.
    std::string big(1024, 'A');
    L.info("T", "%s", big.c_str());
    assert(g_captured.size() == 1);
    assert(g_captured[0].msg.size() <= 255);
    L.clearSink();
    std::cout << "PASS" << std::endl;
}

void test_singleton_returns_same_instance() {
    std::cout << "\n=== Test: getLog() Is Singleton ===" << std::endl;
    Log& a = Log::getLog();
    Log& b = Log::getLog();
    assert(&a == &b);
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running Log Tests ===" << std::endl;
    test_level_filtering();
    test_sink_registration_and_clearing();
    test_sink_context_pointer_passes_through();
    test_sink_called_within_emit();
    test_long_message_truncated_at_buffer();
    test_singleton_returns_same_instance();
    std::cout << "\n=== Log Tests Completed Successfully ===" << std::endl;
    return 0;
}

#endif // ARDUINO
