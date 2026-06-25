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

namespace
{

const std::vector<std::pair<std::string, std::string>> ARDUINO_GUARDED_FILES = {
    { "src/al/hal/EspHal.h", "c++14" },
    { "src/al/web/AutoLinkWeb.cpp", "c++14" },
    { "src/al/pingpong/PingPongMain.h", "c++17" },
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

std::string readFile(const std::string &path)
{
    std::ifstream f(path);
    if (!f.is_open())
        return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string projectRoot()
{
    std::string base = ".";
    for (int i = 0; i < 8; i++) {
        std::ifstream pf(base + "/AGENTS.md");
        if (pf.good())
            return base;
        base += "/..";
    }
    return ".";
}

bool installStub(const std::string &root)
{
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

void test_stub_covers_referenced_symbols()
{
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

void test_arduino_guarded_files_parse()
{
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

} // namespace

int main()
{
    std::cout << "=== Running Compile-Check Tests ===" << std::endl;

    std::string root = projectRoot();
    if (!installStub(root)) {
        std::cout << "  FAIL: installStub failed\n";
        assert(false);
    }
    test_stub_covers_referenced_symbols();
    test_arduino_guarded_files_parse();
    std::cout << "\n=== Compile-Check Tests Completed  Successfully ==="
              << std::endl;
    return 0;
}

#endif