// Compile-check for ARDUINO-gated code.
#ifndef ARDUINO

#    include <cassert>
#    include <cstdio>
#    include <cstdlib>
#    include <cstring>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>
#    include <sys/stat.h>
#    include <tuple>
#    include <unistd.h>
#    include <vector>
#    include "NullArqCache.h"

namespace {

// Third tuple element: extra `-include`
// headers needed for the file to compile
// standalone. EspHal.h's body calls Link
// methods (onRx / onBreak / onTimer /
// begin) but no longer `#include`s
// Link.h — the HAL layer must not depend
// on the link layer. In production TUs
// include/AutoLink.h pulls Link.h in;
// the standalone parse has to do it
// itself.
const std::vector<std::tuple<std::string, std::string, std::string>>
    ARDUINO_GUARDED_FILES = {
        { "src/al/hal/EspHal.h", "c++14", "al/link/Link.h" },
        // AutoLinkWeb split into lifecycle + handlers
        // since the last refactor; both TUs are
        // #ifdef ARDUINO and must parse cleanly under
        // the host stubs.
        { "src/al/web/AutoLinkWeb.cpp", "c++14", "" },
        { "src/al/web/AutoLinkWebHandlers.cpp", "c++14", "" },
        { "src/al/pingpong/PingPongMain.h", "c++17", "" },
        { "src/al/pingpong/Ping.h", "c++17", "" },
    };

const std::vector<std::string> EXPECTED_STUB_SYMBOLS = {
    "uart_driver_install",
    "uart_param_config",
    "uart_set_pin",
    "uart_set_baudrate",
    "uart_driver_delete",
    "gpio_set_pull_mode",
    "esp_timer_start_periodic",
    "httpd_register_uri_handler",
    "httpd_start",
    "httpd_stop",
    "esp_err_to_name",
    "xTaskCreatePinnedToCore",
    "xStreamBufferCreate",
    "esp_random",
    "esp_restart"
};

std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.is_open())
        return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string projectRoot() {
    std::string base = ".";
    for (int i = 0; i < 8; i++) {
        std::ifstream pf(base + "/AGENTS.md");
        if (pf.good())
            return base;
        base += "/..";
    }
    return ".";
}

bool installStub(const std::string &root) {
    std::string src =
        readFile(root + "/test/scripts/env/arduino_stub_template.h");
    if (src.empty())
        return false;

    FILE *f = fopen("/tmp/Arduino.h", "w");
    if (!f)
        return false;
    fputs(src.c_str(), f);
    fclose(f);

    int rc =
        system(("python3 " + root + "/test/scripts/env/install_system_stubs.py")
                   .c_str());
    return rc == 0;
}

void test_stub_covers_referenced_symbols() {
    std::cout
        << "\n=== Test: stub headers cover all ESP-IDF symbols  used by guarded files ==="
        << std::endl;

    std::string combined;
    std::ifstream a("/tmp/Arduino.h");
    if (a.good()) {
        std::stringstream ss;
        ss << a.rdbuf();
        combined += ss.str();
    }

    FILE *findp =
        popen("find /tmp/include -name '*.h' -type f 2>/dev/null", "r");
    if (findp) {
        char buf[512];
        while (fgets(buf, sizeof(buf), findp)) {
            std::string path = buf;
            size_t nl = path.find('\n');
            if (nl != std::string::npos)
                path.resize(nl);
            std::ifstream f(path);
            if (f.good()) {
                std::stringstream ss;
                ss << f.rdbuf();
                combined += ss.str();
            }
        }
        pclose(findp);
    }
    if (combined.empty()) {
        std::cout << "  FAIL: no stubs found (run installStub first)\n";
        assert(false);
    }
    int missing = 0;
    for (const auto &sym : EXPECTED_STUB_SYMBOLS) {
        if (combined.find(sym) == std::string::npos) {
            std::cout << "  FAIL: stub missing " << sym << "\n";
            missing++;
        }
    }
    assert(missing == 0);
    std::cout << "  PASS (stubs declare " << EXPECTED_STUB_SYMBOLS.size()
              << " expected symbols)\n";
}

void test_arduino_guarded_files_parse() {
    std::cout << "\n=== Test: ARDUINO-gated source files  syntax-check ==="
              << std::endl;
    std::string root = projectRoot();
    if (!installStub(root)) {
        std::cout << "  FAIL: could not install stub to  /tmp/Arduino.h\n";
        assert(false);
    }
    int failures = 0;
    for (const auto &entry : ARDUINO_GUARDED_FILES) {
        const std::string &rel = std::get<0>(entry);
        const std::string &std = std::get<1>(entry);
        const std::string &extraInclude = std::get<2>(entry);
        std::string abs = root + "/" + rel;
        std::string includeArg =
            extraInclude.empty() ? "" : " -include " + extraInclude;
        std::string cmd = "g++ -std=" + std +
            " -fsyntax-only -Wall -Wno-unused  -DARDUINO=10607 -DAUTOLINK_USE_ESP_TIMER  -I" +
            root + "/src  -I" + root + "/src/al  -I" + root +
            "/include  -I/tmp  -I/tmp/include  -x c++ -include /tmp/Arduino.h" +
            includeArg + " -c -o /dev/null " + abs + " 2>&1";
        FILE *p = popen(cmd.c_str(), "r");
        if (!p) {
            std::cout << "  FAIL: cannot popen g++ for " << rel << "\n";
            failures++;
            continue;
        }
        std::string out;
        char buf[1024];
        while (fgets(buf, sizeof(buf), p))
            out += buf;
        pclose(p);

        if (out.find("error:") != std::string::npos) {
            std::cout << "  FAIL: " << rel << " has syntax errors:\n";
            std::cout << "  >>> full output:\n" << out << "\n";
            failures++;
        } else {
            std::cout << "  PASS: " << rel << " parses cleanly\n";
        }
    }
    unlink("/tmp/Arduino.h");
    assert(failures == 0);
    std::cout << "  PASS\n";
}

// Pins the Link god-class split. Each new
// helper header must still expose its
// class name. Renaming the class or moving
// it to a different file breaks this gate;
void test_autolink_header_has_no_esp_hal_stubs() {
    // EspHal / EspBlinkHal host stubs live
    // in test/common/EspHalStub.h. The
    // public header must not define
    // either struct inline, regardless
    // of build mode. Pasting the stub
    // back into include/AutoLink.h
    // breaks this gate at file-read
    // time, before any compile.
    std::cout << "\n=== Test: include/AutoLink.h has no ESP HAL stubs ==="
              << std::endl;
    std::string root = projectRoot();
    std::ifstream f(root + "/include/AutoLink.h");
    if (!f.is_open()) {
        std::cout << "  FAIL: cannot open include/AutoLink.h\n";
        assert(false);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    int hits = 0;
    auto check = [&](const std::string &needle) {
        if (ss.str().find(needle) != std::string::npos) {
            std::cout << "  FAIL: include/AutoLink.h still contains '" << needle
                      << "'\n";
            hits++;
        }
    };
    check("struct EspHal : public IHal");
    check("struct EspBlinkHal {");
    std::ifstream stub(root + "/test/common/EspHalStub.h");
    if (!stub.is_open()) {
        std::cout << "  FAIL: test/common/EspHalStub.h missing\n";
        hits++;
    }
    assert(hits == 0);
    std::cout << "  PASS (include/AutoLink.h has no inline HAL stubs; "
              << "test/common/EspHalStub.h present)\n";
}

void test_link_has_no_arq_trampoline_pointers() {
    // ARQ cache was lifted out of the
    // AutoLink facade. The 6-pointer
    // trampoline chain in
    // Link.h is gone. Toggling any
    // of these names back into Link.h
    // (or moving the IArqCache* member
    // out) breaks the structural pin.
    std::cout << "\n=== Test: Link has no ARQ trampoline pointers ==="
              << std::endl;
    std::string root = projectRoot();
    std::ifstream f(root + "/src/al/link/Link.h");
    if (!f.is_open()) {
        std::cout << "  FAIL: cannot open src/al/link/Link.h\n";
        assert(false);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    auto mustNotContain = [&](const std::string &needle) {
        if (ss.str().find(needle) != std::string::npos) {
            std::cout << "  FAIL: Link.h still contains '" << needle << "'\n";
            return false;
        }
        return true;
    };
    int hits = 0;
    auto check = [&](const std::string &needle) {
        if (!mustNotContain(needle)) {
            std::cerr << "  Link.h still has '" << needle << "'\n";
            std::cerr.flush();
            hits++;
        }
    };
    check("ArqAckCallback");
    check("ArqRetxCallback");
    check("ArqCacheHasRoomCallback");
    check("ArqCacheInsertCallback");
    check("ArqCacheClearAllCallback");
    check("LinkResetCallback");
    check("arqAckCallback_");
    check("arqRetxCallback_");
    check("arqCacheHasRoomCallback_");
    check("arqCacheInsertCallback_");
    check("arqCacheClearAllCallback_");
    check("linkResetCallback_");
    check("arqCtx_");
    check("setArqCacheHooks");
    check("setLinkResetHook");
    assert(hits == 0);
    std::cout << "  PASS (Link.h has no trampoline pointers or setters)\n";
}

// reverting the split (consolidating back
// into Link.h) breaks it at compile time.
void test_link_ctor_requires_arq_cache_reference() {
    // The Link ctor must take IArqCache&
    // (reference, not pointer) and there
    // must be no public setArqCache()
    // method. Reference semantics make
    // the cache-outlives-link contract
    // unbreakable at compile time;
    // reintroducing a raw IArqCache*
    // member or a public setter silently
    // produces a dangling pointer if a
    // future refactor moves the cache to
    // unique_ptr or reorders members.
    //
    // Toggling IArqCache& -> IArqCache*
    // in the Link ctor signature breaks
    // test 1. Reintroducing
    // `void setArqCache(IArqCache *c)`
    // in the public section breaks test 2.
    // Changing IArqCache &arqCache_ back
    // to IArqCache *arqCache_ breaks
    // test 3.
    std::cout
        << "\n=== Test: Link ctor takes IArqCache& (reference, not pointer) ==="
        << std::endl;
    std::string root = projectRoot();
    std::ifstream f(root + "/src/al/link/Link.h");
    if (!f.is_open()) {
        std::cout << "  FAIL: cannot open src/al/link/Link.h\n";
        assert(false);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();
    int hits = 0;
    auto mustContain = [&](const std::string &needle) {
        if (content.find(needle) == std::string::npos) {
            std::cout << "  FAIL: Link.h missing '" << needle << "'\n";
            hits++;
        }
    };
    auto mustNotContain = [&](const std::string &needle) {
        if (content.find(needle) != std::string::npos) {
            std::cout << "  FAIL: Link.h still contains '" << needle << "'\n";
            hits++;
        }
    };
    // The ctor signature must take an
    // IArqCache& reference (the `&`
    // after IArqCache) and a bool
    // isMasterNode right after.
    mustContain("Link(IHal &hw, IArqCache &cache, bool isMasterNode");
    // The member must be a reference:
    // `IArqCache &arqCache_;`. A raw
    // pointer (`IArqCache *arqCache_`)
    // or no-default member would let
    // the lifetime break silently.
    mustContain("IArqCache &arqCache_;");
    // The post-construction setter
    // must be gone. The public API
    // surface is what production
    // sketches can call; a `private:`
    // friend setter (none today) would
    // not break this pin because the
    // literal name is absent.
    mustNotContain("void setArqCache(");
    // The default-initialised pointer
    // (nullptr default) must also be
    // gone; with a reference member,
    // the cache is bound at construction.
    mustNotContain("IArqCache *arqCache_");
    assert(hits == 0);
    std::cout
        << "  PASS (Link ctor binds cache by reference; setArqCache gone)\n";
}

void test_linkarq_linkreorder_linksweep_present() {
    std::cout << "\n=== Test: LinkArq / LinkReorder / LinkSweep present ==="
              << std::endl;
    std::string root = projectRoot();
    int missing = 0;
    auto contains = [&](const std::string &rel, const std::string &needle) {
        std::string abs = root + "/" + rel;
        std::ifstream f(abs);
        if (!f.is_open()) {
            std::cout << "  FAIL: cannot open " << rel << "\n";
            return false;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        bool ok = ss.str().find(needle) != std::string::npos;
        if (!ok) {
            std::cout << "  FAIL: " << rel << " missing '" << needle << "'\n";
            return false;
        }
        return true;
    };
    if (!contains("src/al/link/arq/LinkArq.h", "class LinkArq"))
        missing++;
    if (!contains("src/al/link/LinkReorder.h", "class LinkReorder"))
        missing++;
    if (!contains("src/al/link/sweep/LinkSweep.h", "class LinkSweep"))
        missing++;
    if (!contains("src/al/link/arq/IArqCache.h", "class IArqCache"))
        missing++;
    if (!contains("src/al/link/arq/ArqCache.h", "class ArqCache"))
        missing++;
    assert(missing == 0);
    std::cout << "  PASS (all five classes present in their headers)\n";
}

// Pin the dead-code cleanup boundary.
// Each pin reads a source file and
// asserts a symbol that was deleted
// is gone. Reintroducing any of these
// names (with a matching definition or
// a stray write site) breaks the
// structural pin. Per AGENTS.md rule
// 18: dead-code removal is a fix and
// needs a regression test that fails
// when the fix is reverted.
void test_dead_code_boundary() {
    std::cout << "\n=== Test: dead-code boundary pins ===" << std::endl;
    std::string root = projectRoot();
    auto contains = [&](const std::string &rel, const std::string &needle) {
        std::string abs = root + "/" + rel;
        std::ifstream f(abs);
        if (!f.is_open()) {
            std::cout << "  FAIL: cannot open " << rel << "\n";
            return false;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str().find(needle) != std::string::npos;
    };
    auto absent = [&](const std::string &rel, const std::string &needle) {
        if (contains(rel, needle)) {
            std::cout << "  FAIL: " << rel << " still contains '" << needle
                      << "'\n";
            return false;
        }
        return true;
    };
    int hits = 0;
    auto chk = [&](const std::string &rel, const std::string &needle) {
        if (!absent(rel, needle))
            hits++;
    };

    // #1: Link::computeDwells_unlocked
    // ghost declaration in Link.h.
    // Live computeDwells is on
    // LinkSweep, not Link.
    chk("src/al/link/Link.h", "computeDwells_unlocked");

    // #2: retxNeeded_ write-only flag.
    // Field decl in Link.h, two write
    // sites in Link.cpp (onNak and the
    // ARQ retx loop).
    chk("src/al/link/Link.h", "retxNeeded_");
    // The split refactor moved Link.cpp's
    // method bodies across LinkCore / LinkTx /
    // LinkRx / LinkSweep / LinkTimers / LinkApi
    // — so dead-code searches must cover all six.
    for (const char *tu : {"LinkCore.cpp", "LinkTx.cpp", "LinkRx.cpp",
                           "LinkSweep.cpp", "LinkTimers.cpp", "LinkApi.cpp"}) {
        chk((std::string("src/al/link/") + tu).c_str(), "retxNeeded_");
    }

    // #3: Link::sendFrame() locking
    // wrapper. The unlocked helper
    // sendFrame_unlocked stays. Pin:
    // there must be no definition
    // `void Link::sendFrame(` (no
    // _unlocked suffix).
    for (const char *tu : {"LinkCore.cpp", "LinkTx.cpp", "LinkRx.cpp",
                           "LinkSweep.cpp", "LinkTimers.cpp", "LinkApi.cpp"}) {
        chk((std::string("src/al/link/") + tu).c_str(),
            "void Link::sendFrame(");
    }

    // #4: Link::popRetransmitSlot()
    // and LinkArq::popRetransmitSlot()
    // (Link-level wrapper + helper
    // both unreachable; retx path
    // uses arq_.decideSlot + applyRetx
    // directly).
    chk("src/al/link/Link.h", "popRetransmitSlot");
    for (const char *tu : {"LinkCore.cpp", "LinkTx.cpp", "LinkRx.cpp",
                           "LinkSweep.cpp", "LinkTimers.cpp", "LinkApi.cpp"}) {
        chk((std::string("src/al/link/") + tu).c_str(), "popRetransmitSlot");
    }
    chk("src/al/link/arq/LinkArq.h", "popRetransmitSlot");
    chk("src/al/link/arq/LinkArq.cpp", "popRetransmitSlot");

    // #5: LinkArq::clearSlot. Per-slot
    // clearing happens via onAcked /
    // clearAll; the standalone
    // method is uncalled.
    chk("src/al/link/arq/LinkArq.h", "clearSlot");
    chk("src/al/link/arq/LinkArq.cpp", "clearSlot");

    // #6: LinkSweep::enterResweep.
    // Resweep path in reset_unlocked
    // goes to enterPhase1 directly;
    // the baud-preference helper is
    // gone with its accessors
    // (preferredBaudIndex, baudRetryLimit,
    // baudRetries, incBaudRetries,
    // clearBaudRetries, clearPreferredBaud).
    chk("src/al/link/sweep/LinkSweep.h", "enterResweep");
    chk("src/al/link/sweep/LinkSweep.cpp", "enterResweep");
    chk("src/al/link/Link.h", "preferredBaudIndex");
    chk("src/al/link/Link.h", "baudRetryLimit()");
    chk("src/al/link/Link.h", "incBaudRetries");
    chk("src/al/link/Link.h", "clearBaudRetries");
    chk("src/al/link/Link.h", "clearPreferredBaud");

    // #7: AutoLinkConfig::baudPreference
    // and baudRetryLimit (unread config
    // fields; no production code reads
    // them after enterResweep is gone).
    chk("src/al/link/Link.h", "baudPreference");
    chk("src/al/link/Link.h", "baudRetryLimit");

    // #8: swpRxBytes — written in onRx()
    // during SWP, cleared in
    // reset_unlocked(), never read,
    // never exposed in Diag or Stats.
    chk("src/al/link/Link.h", "swpRxBytes");
    for (const char *tu : {"LinkCore.cpp", "LinkTx.cpp", "LinkRx.cpp",
                           "LinkSweep.cpp", "LinkTimers.cpp", "LinkApi.cpp"}) {
        chk((std::string("src/al/link/") + tu).c_str(), "swpRxBytes");
    }

    assert(hits == 0);
    std::cout << "  PASS (all 8 dead-code removals pinned)\n";
}

} // namespace

int main() {
    std::cout << "=== Running Compile-Check Tests ===" << std::endl;

    std::string root = projectRoot();
    if (!installStub(root)) {
        std::cout << "  FAIL: installStub failed\n";
        assert(false);
    }
    test_stub_covers_referenced_symbols();
    test_arduino_guarded_files_parse();
    test_linkarq_linkreorder_linksweep_present();
    test_link_has_no_arq_trampoline_pointers();
    test_autolink_header_has_no_esp_hal_stubs();
    test_link_ctor_requires_arq_cache_reference();
    test_dead_code_boundary();
    std::cout << "\n=== Compile-Check Tests Completed  Successfully ==="
              << std::endl;
    return 0;
}

#endif