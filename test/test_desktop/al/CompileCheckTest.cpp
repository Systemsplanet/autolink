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
#    include <unistd.h>
#    include <vector>

namespace {

const std::vector<std::pair<std::string, std::string>> ARDUINO_GUARDED_FILES = {
    { "src/al/hal/EspHal.h", "c++14" },
    { "src/al/web/AutoLinkWeb.cpp", "c++14" },
    { "src/al/pingpong/PingPongMain.h", "c++17" },
    { "src/al/pingpong/Ping.h", "c++17" },
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
        readFile(root + "/test/test_desktop/arduino_stub_template.h");
    if (src.empty())
        return false;

    FILE *f = fopen("/tmp/Arduino.h", "w");
    if (!f)
        return false;
    fputs(src.c_str(), f);
    fclose(f);

    int rc = system(
        ("python3 " + root + "/test/test_desktop/install_system_stubs.py")
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
        const std::string &rel = entry.first;
        const std::string &std = entry.second;
        std::string abs = root + "/" + rel;
        std::string cmd = "g++ -std=" + std +
            " -fsyntax-only -Wall -Wno-unused  -DARDUINO=10607 -DAUTOLINK_USE_ESP_TIMER  -I" +
            root + "/src  -I" + root + "/src/al  -I" + root +
            "/include  -I/tmp  -I/tmp/include  -x c++ -include /tmp/Arduino.h  -c -o /dev/null " +
            abs + " 2>&1";
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
            std::istringstream iss(out);
            std::string line;
            int printed = 0;
            while (std::getline(iss, line) && printed < 8) {
                if (line.find("error:") != std::string::npos) {
                    std::cout << "    " << line << "\n";
                    printed++;
                }
            }
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
    if (!contains("src/al/link/LinkArq.h", "class LinkArq"))
        missing++;
    if (!contains("src/al/link/LinkReorder.h", "class LinkReorder"))
        missing++;
    if (!contains("src/al/link/LinkSweep.h", "class LinkSweep"))
        missing++;
    if (!contains("src/al/link/IArqCache.h", "class IArqCache"))
        missing++;
    if (!contains("src/al/link/ArqCache.h", "class ArqCache"))
        missing++;
    assert(missing == 0);
    std::cout << "  PASS (all five classes present in their headers)\n";
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
    std::cout << "\n=== Compile-Check Tests Completed  Successfully ==="
              << std::endl;
    return 0;
}

#endif