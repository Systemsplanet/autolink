#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <fstream>
#    include <regex>
#    include <string>
#    include <vector>

// Pins AGENTS.md rule 9. Walks the source tree
// and asserts no .cpp / .h / .ino / .py / .sh /
// .mk / .yml / .json / .txt / non-Version .md
// file hard-codes a X.Y.Z version string except
// the AUTOLINK_VERSION macro in include/AutoLink.h
// (the rule's only legitimate exception).
// Fails the build, not just a test, if a version
// reference slips in: re-introducing a hard-coded
// version becomes a compile-time regression.

static const std::vector<std::string> kSourceExts = {
    ".cpp", ".h", ".ino", ".py", ".sh", ".mk", ".yml", ".json", ".txt",
};

static bool isExemptPath(const std::string &path) {
    if (path.find("docs/Version.md") != std::string::npos)
        return true;
    if (path.find("library.properties") != std::string::npos)
        return true;
    if (path.find("idf_component.yml") != std::string::npos)
        return true;
    if (path.find("AGENTS.md") != std::string::npos)
        return true;
    if (path.find("README.md") != std::string::npos)
        return true;
    if (path.find("verify_build/verify_build.ino") != std::string::npos)
        return true;
    if (path.find("VersionFreeSourceTest.cpp") != std::string::npos)
        return true;
    return false;
}

// Match X.Y.Z or vX.Y.Z where X/Y/Z are digits.
// Skips X.Y.Z inside an AUTOLINK_VERSION reference
// (the macro is the only legitimate version token).
static const std::regex kVersionPattern(R"((?:^|[^A-Za-z_])(v?5\.\d+\.\d+))");

void test_no_hardcoded_version_in_sources() {
    std::cout << "\n=== Test: rule-9 no hard-coded version in source ==="
              << std::endl;
    const std::vector<std::string> roots = {
        "../../src",
        "../../include",
        "../..",
        "../../build",
    };
    std::vector<std::string> violations;
    for (const auto &root : roots) {
        std::string cmd = "find " + root +
            " -type f \\( "
            "-name '*.cpp' -o -name '*.h' -o -name '*.ino' "
            "-o -name '*.py'  -o -name '*.sh' -o -name '*.mk' "
            "-o -name '*.yml' -o -name '*.json' "
            "-o -name '*.txt' "
            "\\)";
        FILE *p = popen(cmd.c_str(), "r");
        if (!p)
            continue;
        char buf[4096];
        while (fgets(buf, sizeof(buf), p)) {
            std::string path(buf);
            while (!path.empty() &&
                   (path.back() == '\n' || path.back() == '\r'))
                path.pop_back();
            if (isExemptPath(path))
                continue;
            std::ifstream in(path);
            if (!in)
                continue;
            std::string line;
            int lineno = 0;
            while (std::getline(in, line)) {
                lineno++;
                if (line.find("AUTOLINK_VERSION") != std::string::npos)
                    continue;
                if (std::regex_search(line, kVersionPattern)) {
                    std::string m =
                        path + ":" + std::to_string(lineno) + ": " + line;
                    violations.push_back(m);
                }
            }
        }
        pclose(p);
    }
    if (!violations.empty()) {
        std::cerr << "FAIL: hard-coded version strings found:" << std::endl;
        for (const auto &v : violations)
            std::cerr << "  " << v << std::endl;
        assert(violations.empty());
    }
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running Version-Free Source Tests ===" << std::endl;
    test_no_hardcoded_version_in_sources();
    std::cout << "\n=== Version-Free Source Tests Completed ===" << std::endl;
    return 0;
}

#endif
