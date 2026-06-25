// ESP-IDF esp_err_t return-check audit.
#ifndef ARDUINO

#    include <cassert>
#    include <cctype>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>
#    include <vector>

namespace {

struct CallSite {
    std::string file;
    std::string function;
    std::string requireLogContains;
};

const std::vector<CallSite> REQUIRED_SITES = {
    { "src/al/hal/EspHal.h", "uart_driver_install", "esp_err_to_name" },
    { "src/al/hal/EspHal.h", "uart_param_config", "esp_err_to_name" },
    { "src/al/hal/EspHal.h", "uart_set_pin", "esp_err_to_name" },
    { "src/al/hal/EspHal.h", "uart_set_baudrate", "esp_err_to_name" },
    { "src/al/hal/EspHal.h", "gpio_set_pull_mode", "esp_err_to_name" },
    { "src/al/web/AutoLinkWeb.cpp", "httpd_register_uri_handler",
      "esp_err_to_name" },
    { "src/al/web/AutoLinkWeb.cpp", "httpd_start", "esp_err_to_name" },
    { "src/al/web/AutoLinkWeb.cpp", "esp_timer_create", "esp_err_to_name" },
    { "src/al/web/AutoLinkWeb.cpp", "esp_timer_start_periodic",
      "esp_err_to_name" },
    { "src/al/web/AutoLinkWeb.cpp", "esp_timer_stop", "esp_err_to_name" },
    { "src/al/web/AutoLinkWeb.cpp", "esp_timer_delete", "esp_err_to_name" },
    { "src/al/web/AutoLinkWeb.cpp", "httpd_stop", "esp_err_to_name" },
};

std::string resolvePath(const std::string &rel) {
    namespace fs = std;
    std::string base = ".";
    for (int i = 0; i < 8; i++) {
        std::string probe = base + "/AGENTS.md";
        std::ifstream pf(probe);
        if (pf.good()) {
            return base + "/" + rel;
        }
        base += "/..";
    }
    return rel;
}

std::string readFile(const std::string &path) {
    std::string abs = resolvePath(path);
    std::ifstream f(abs);
    if (!f.is_open())
        return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> splitLines(const std::string &src) {
    std::vector<std::string> lines;
    std::istringstream iss(src);
    std::string line;
    while (std::getline(iss, line))
        lines.push_back(line);
    return lines;
}

bool lineContainsToken(const std::string &line, const std::string &token) {
    size_t pos = 0;
    while ((pos = line.find(token, pos)) != std::string::npos) {
        bool leftOk = (pos == 0) ||
            (!std::isalnum(static_cast<unsigned char>(line[pos - 1])) &&
             line[pos - 1] != '_');
        bool rightOk = (pos + token.size() >= line.size()) ||
            (!std::isalnum(
                 static_cast<unsigned char>(line[pos + token.size()])) &&
             line[pos + token.size()] != '_');
        if (leftOk && rightOk)
            return true;
        pos += token.size();
    }
    return false;
}

int everyCallHasNearbyLog(const std::vector<std::string> &lines,
                          const std::string &fn, const std::string &logToken,
                          size_t window = 15) {
    size_t n = lines.size();
    size_t callCount = 0;
    size_t mismatches = 0;
    for (size_t i = 0; i < n; i++) {
        if (!lineContainsToken(lines[i], fn))
            continue;
        callCount++;
        size_t end = std::min(n, i + window);
        bool found = false;
        for (size_t j = i; j < end; j++) {
            if (lineContainsToken(lines[j], logToken)) {
                found = true;
                break;
            }
        }
        if (!found) {
            std::cout << "  FAIL: " << fn << " at line " << (i + 1)
                      << " has no " << logToken << " within " << window
                      << " lines\n";
            mismatches++;
        }
    }
    if (callCount == 0)
        return -1;
    return static_cast<int>(mismatches);
}

void test_required_call_sites_have_err_to_name_log() {
    std::cout << "\n=== Test: ESP-IDF error sites have esp_err_to_name log ==="
              << std::endl;

    int failures = 0;
    for (const auto &site : REQUIRED_SITES) {
        std::string src = readFile(site.file);
        if (src.empty()) {
            std::cout << "  FAIL: cannot read " << site.file << "\n";
            failures++;
            continue;
        }
        auto lines = splitLines(src);
        int rc = everyCallHasNearbyLog(lines, site.function,
                                       site.requireLogContains);
        if (rc == -1) {
            std::cout << "  FAIL: " << site.file << " — " << site.function
                      << " not called at all\n";
            failures++;
        } else if (rc > 0) {
            std::cout << "  FAIL: " << site.file << " — " << site.function
                      << " has " << rc << " call(s) missing "
                      << site.requireLogContains << "\n";
            failures++;
        } else {
            std::cout << "  PASS: " << site.file << " — " << site.function
                      << " logged via " << site.requireLogContains << "\n";
        }
    }
    assert(failures == 0);
    std::cout << "  PASS\n";
}

void test_checker_detects_missing_log() {
    std::cout
        << "\n=== Test: checker flags missing esp_err_to_name (toggle verification) ==="
        << std::endl;

    std::string bad =
        "void f() {\n    uart_driver_install(1, 1024, 1024, 10, &q, 0);\n}\n";
    auto lines = splitLines(bad);
    int rc =
        everyCallHasNearbyLog(lines, "uart_driver_install", "esp_err_to_name");
    assert(rc > 0);
    std::cout << "  PASS (checker correctly flagged the bare call)\n";
}

} // namespace

int main() {
    std::cout << "=== Running ESP-IDF Error-Etiquette Tests ===" << std::endl;
    test_required_call_sites_have_err_to_name_log();
    test_checker_detects_missing_log();
    std::cout
        << "\n=== ESP-IDF Error-Etiquette Tests Completed Successfully ==="
        << std::endl;
    return 0;
}

#endif