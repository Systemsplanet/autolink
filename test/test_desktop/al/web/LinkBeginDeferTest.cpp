// Auto-generated split of the original
// HandleRootChunkedTest.cpp. Each TU in this
// split covers a single concern (handleRoot
// chunked send / begin lifecycle / httpd retry
// budget / Link kickoff defer) and includes
// the shared helpers via
// HandleRootChunkedTestCommon.h.
#ifndef ARDUINO

#    include "HandleRootChunkedTestCommon.h"
#    include "al/util/Log.h"

using namespace autolink;

void test_link_begin_defers_kickoff_when_paused() {
    std::cout << "\n=== Link::begin() defers kickoff when linkPaused_ ==="
              << std::endl;
    std::string hSrc = readFile(projectRoot() + "/src/al/link/Link.h");
    // Link::begin() and Link::kickoff() both live in
    // LinkCore.cpp after the god-class split.
    std::string cSrc = readFile(projectRoot() + "/src/al/link/LinkCore.cpp");
    assert(!hSrc.empty());
    assert(!cSrc.empty());

    // Public declaration exists.
    assert(hSrc.find("void kickoff();") != std::string::npos);

    // kickoff() implementation exists.
    assert(cSrc.find("void Link::kickoff()") != std::string::npos);

    // kickoff() must be idempotent (kickedOff_ guard).
    auto kickoffBody = extractFnBody(cSrc, "void Link::kickoff()");
    assert(!kickoffBody.empty());
    assert(kickoffBody.find("kickedOff_") != std::string::npos);

    // Link::begin() must check linkPaused_ and skip the
    // kickoff when paused.
    auto beginBody = extractFnBody(cSrc, "void Link::begin()");
    assert(!beginBody.empty());
    assert(beginBody.find("linkPaused_") != std::string::npos);

    // The kickedOff_ flag is set false at the start of begin().
    assert(beginBody.find("kickedOff_ = false") != std::string::npos);

    // The branch shape must be "if (linkPaused_) return;
    // kickoff();" — i.e. when paused, begin() does NOT
    // call kickoff. Find an early-return guarded by
    // linkPaused_ that precedes the kickoff() call.
    auto pausedCheck = beginBody.find("linkPaused_)");
    auto kickoffCall = beginBody.find("\n    kickoff();");
    assert(pausedCheck != std::string::npos);
    assert(kickoffCall != std::string::npos);
    assert(pausedCheck < kickoffCall);
    // The early-return must be inside the same function
    // (between pausedCheck and kickoffCall).
    auto earlyReturn = beginBody.find("return;", pausedCheck);
    assert(earlyReturn != std::string::npos);
    assert(earlyReturn < kickoffCall);

    std::cout << "  PASS (Link::kickoff is public + idempotent; "
                 "begin() defers when linkPaused_)\n";
}

// Pin: Ping::setup() falls through to auto-kickoff when
// the web monitor never came up. The user explicitly
// asked for this: if the GUI can't start, don't leave
// the device silent — drive the wire.

void test_ping_falls_through_when_gui_down() {
    std::cout << "\n=== Ping::setup falls through to kickoff when "
                 "GUI is down ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/pingpong/Ping.h");
    assert(!src.empty());

    auto setupBody = extractFnBody(src, "void setup()");
    assert(!setupBody.empty());

    // The fall-through branch must check the web monitor's
    // isUp() predicate.
    assert(setupBody.find("isUp()") != std::string::npos);
    // And it must call kickoff() on the comm to drive
    // the wire even when the GUI is not up.
    assert(setupBody.find("comm_.kickoff()") != std::string::npos);
    // And it must flip paused_ off so loop() actually
    // sends (not just sits in paused mode).
    assert(setupBody.find("paused_ = false") != std::string::npos);

    std::cout << "  PASS (Ping falls through to kickoff when mon.isUp() "
                 "is false)\n";
}

#endif

int main() {
    std::cout << "=== Running LinkBeginDeferTest ===" << std::endl;

    Log::log().setLevel(Log::DEBUG);
    test_link_begin_defers_kickoff_when_paused();
    test_ping_falls_through_when_gui_down();

    std::cout << "\n=== LinkBeginDeferTest Completed ===" << std::endl;
    return 0;
}
