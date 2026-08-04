// Link and AutoLink expose no test hooks on their public API. Host
// tests reach internals only through the friend shims in
// test/common/. Source-level by necessity: a public hook's presence
// IS the defect, so there is nothing to observe at runtime.
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see Makefile)"
#endif

#include "al/link/Link.h"
#include "AutoLink.h"
#include "LinkTestAccessor.h"
#include "AutoLinkTestAccessor.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace autolink;

namespace {

std::string slurp(const std::string &path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "FAIL: cannot open " << path << std::endl;
        assert(false);
    }
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

// Everything between a `public:` and the next `private:` (or the end
// of the class).
std::vector<std::string> publicBodies(const std::string &src) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < src.size()) {
        size_t pub = src.find("public:", pos);
        if (pub == std::string::npos)
            break;
        size_t end = src.find("private:", pub + 7);
        if (end == std::string::npos)
            end = src.size();
        out.emplace_back(src.substr(pub, end - pub));
        pos = end;
    }
    return out;
}

// A `test_` or `ForTest` token followed by a call signature. Catches
// hook names a literal list would miss.
bool declaresHook(const std::vector<std::string> &bodies) {
    static const char *needles[] = { "test_", "ForTest" };
    for (const auto &s : bodies) {
        for (const char *needle : needles) {
            const size_t nlen = std::string(needle).size();
            size_t pos = 0;
            while ((pos = s.find(needle, pos)) != std::string::npos) {
                size_t paren = s.find('(', pos + nlen);
                if (paren != std::string::npos && paren - pos <= 80)
                    return true;
                pos += nlen;
            }
        }
    }
    return false;
}

void checkNoHooks(const char *path, const char *what) {
    auto bodies = publicBodies(slurp(path));
    assert(!bodies.empty() && "no public section found — wrong path?");
    assert(!declaresHook(bodies) &&
           "public API declares a test_* / *ForTest hook; move it behind "
           "the friend shim in test/common/");
    std::cout << "  " << what << ": no public test hooks" << std::endl;
}

} // namespace

int main() {
    std::cout << "\n=== Test: no test hooks on the public API ===" << std::endl;
    checkNoHooks("../../src/al/link/Link.h", "Link");
    checkNoHooks("../../include/AutoLink.h", "AutoLink");

    // The shims are the only sanctioned path in: delete either one, or
    // its friend declaration, and this TU stops compiling.
    static_assert(sizeof(LinkTestAccessor) > 0, "LinkTestAccessor missing");
    static_assert(sizeof(AutoLinkTestAccessor) > 0,
                  "AutoLinkTestAccessor missing");
    std::cout << "  shim boundary intact" << std::endl;
    std::cout << "PASS" << std::endl;
    return 0;
}
