// Log sink fan-out, level filter.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <string>
#    include <vector>
#    include <cstdio>
#    include "al/util/Log.h"

using namespace autolink;

struct CapturedLine {
    char sev;
    std::string tag;
    std::string msg;
};
static std::vector<CapturedLine> g_captured;
static void captureSink(char sev, const char *tag, const char *msg, void *)
{
    g_captured.push_back({ sev, tag ? tag : "", msg ? msg : "" });
}

void test_level_filtering()
{
    std::cout << "\n=== Test: Log Level Filtering (4: NONE..DEBUG) ==="
              << std::endl;
    Log &L = Log::log();
    L.setSink(captureSink);
    g_captured.clear();

    L.setLevel(Log::NONE);
    L.error("T", "err_should_not_appear");
    L.info("T", "info_should_not_appear");
    L.debug("T", "dbg_should_not_appear");
    assert(g_captured.size() == 0);

    L.setLevel(Log::ERROR);
    L.error("T", "err1");
    L.info("T", "info1");
    L.debug("T", "dbg1");
    assert(g_captured.size() == 1);
    assert(g_captured[0].sev == 'E' && g_captured[0].msg == "err1");

    L.setLevel(Log::WARNING);
    L.error("T", "err2");
    L.warning("T", "warn2");
    L.info("T", "info2");
    L.debug("T", "dbg2");
    assert(g_captured.size() == 3);
    assert(g_captured[1].sev == 'E' && g_captured[1].msg == "err2");
    assert(g_captured[2].sev == 'W' && g_captured[2].msg == "warn2");

    L.setLevel(Log::INFO);
    L.error("T", "err3");
    L.info("T", "info3");
    L.debug("T", "dbg3");

    assert(g_captured.size() == 5);
    assert(g_captured[3].sev == 'E' && g_captured[3].msg == "err3");
    assert(g_captured[4].sev == 'I' && g_captured[4].msg == "info3");

    L.setLevel(Log::DEBUG);
    L.error("T", "err4");
    L.info("T", "info4");
    L.debug("T", "dbg4");

    assert(g_captured.size() == 8);
    assert(g_captured[5].sev == 'E' && g_captured[5].msg == "err4");
    assert(g_captured[6].sev == 'I' && g_captured[6].msg == "info4");
    assert(g_captured[7].sev == 'D' && g_captured[7].msg == "dbg4");

    L.setLevel(Log::VERBOSE);
    L.error("T", "err5");
    L.info("T", "info5");
    L.debug("T", "dbg5");
    L.verbose("T", "vb5");

    assert(g_captured.size() == 12);
    assert(g_captured[8].sev == 'E' && g_captured[8].msg == "err5");
    assert(g_captured[9].sev == 'I' && g_captured[9].msg == "info5");
    assert(g_captured[10].sev == 'D' && g_captured[10].msg == "dbg5");
    assert(g_captured[11].sev == 'V' && g_captured[11].msg == "vb5");

    assert((int)Log::NONE < (int)Log::ERROR);
    assert((int)Log::ERROR < (int)Log::WARNING);
    assert((int)Log::WARNING < (int)Log::INFO);
    assert((int)Log::INFO < (int)Log::DEBUG);
    assert((int)Log::DEBUG < (int)Log::VERBOSE);

    L.setLevel(Log::WARNING);
    assert(L.wouldEmit(Log::ERROR));
    assert(L.wouldEmit(Log::WARNING));
    assert(!L.wouldEmit(Log::INFO));
    assert(!L.wouldEmit(Log::DEBUG));
    L.setLevel(Log::NONE);
    assert(!L.wouldEmit(Log::ERROR));
    assert(!L.wouldEmit(Log::WARNING));
    assert(!L.wouldEmit(Log::INFO));
    assert(!L.wouldEmit(Log::DEBUG));
    L.setLevel(Log::DEBUG);
    assert(L.wouldEmit(Log::ERROR));
    assert(L.wouldEmit(Log::WARNING));
    assert(L.wouldEmit(Log::INFO));
    assert(L.wouldEmit(Log::DEBUG));

    assert(!L.wouldEmit(Log::VERBOSE));
    L.setLevel(Log::VERBOSE);
    assert(L.wouldEmit(Log::ERROR));
    assert(L.wouldEmit(Log::WARNING));
    assert(L.wouldEmit(Log::INFO));
    assert(L.wouldEmit(Log::DEBUG));
    assert(L.wouldEmit(Log::VERBOSE));

    std::cout << "PASS" << std::endl;
}

void test_sink_registration_and_clearing()
{
    std::cout << "\n=== Test: Sink Registration & Clearing ===" << std::endl;
    Log &L = Log::log();
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

void test_sink_context_pointer_passes_through()
{
    std::cout << "\n=== Test: Sink Context Pointer ===" << std::endl;
    Log &L = Log::log();
    L.setLevel(Log::DEBUG);
    int marker = 0xCAFE;
    auto fn = [](char, const char *, const char *, void *ctx) {
        int *m = reinterpret_cast<int *>(ctx);
        *m = 0xBEEF;
    };
    L.setSink(fn, &marker);
    L.info("T", "x");
    assert(marker == 0xBEEF);
    L.clearSink();
    std::cout << "PASS" << std::endl;
}

void test_sink_called_within_emit()
{
    std::cout << "\n=== Test: Sink Called Within emit() ===" << std::endl;
    Log &L = Log::log();
    L.setLevel(Log::DEBUG);
    L.setSink(captureSink);
    g_captured.clear();
    L.error("X", "emit order");
    L.info("X", "second line");
    assert(g_captured.size() == 2);
    assert(g_captured[0].msg == "emit order");
    assert(g_captured[1].msg == "second line");
    L.clearSink();
    std::cout << "PASS" << std::endl;
}

void test_long_message_truncated_at_buffer()
{
    std::cout << "\n=== Test: Long Message Truncated ===" << std::endl;
    Log &L = Log::log();
    L.setLevel(Log::DEBUG);
    L.setSink(captureSink);
    g_captured.clear();

    std::string big(1024, 'A');
    L.info("T", "%s", big.c_str());
    assert(g_captured.size() == 1);
    assert(g_captured[0].msg.size() <= 384);
    assert(g_captured[0].msg.size() > 320);
    L.clearSink();
    std::cout << "PASS" << std::endl;
}

void test_singleton_returns_same_instance()
{
    std::cout << "\n=== Test: Log::log() Is Singleton ===" << std::endl;
    Log &a = Log::log();
    Log &b = Log::log();
    assert(&a == &b);
    std::cout << "PASS" << std::endl;
}

int main()
{
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

#endif