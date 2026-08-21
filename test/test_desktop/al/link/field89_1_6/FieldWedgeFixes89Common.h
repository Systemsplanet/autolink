// Shared header for the field-wedge
// regression pins (AL89 batch). The
// previous monolithic file (22.7 KB)
// exceeded the per-file 15 KB cap
// (AGENTS.md rule 20a) and bundled 12
// unrelated pins into a single .cpp.
// AL90-17 split each pin into its own
// .cpp under test/test_desktop/al/link/
// field89/. The per-file header holds
// the includes and the readFile /
// projectRoot / stripComments helpers
// the source-grep pins share — every
// field89/ test file #includes this.
#pragma once

#ifndef ARDUINO

#    include <cassert>
#    include <chrono>
#    include <cstdio>
#    include <cstring>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>
#    include <thread>
#    include <vector>

#    include "MockHal.h"
#    include "TestCfg.h"
#    include "LinkTestAccessor.h"
#    include "AutoLinkTestAccessor.h"
#    include "al/link/Link.h"
#    include "al/link/arq/ArqCache.h"
#    include "al/link/timers/LinkHealth.h"
#    include "al/AutoLinkConfig.h"
#    include "al/util/log/Log.h"
#    include "AutoLink.h"
#    include "EspHalStub.h"
#    include "NullArqCache.h"

namespace autolink {
namespace field89 {

inline std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.good())
        return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

inline std::string projectRoot() {
    std::string base = ".";
    for (int i = 0; i < 8; i++) {
        std::ifstream pf(base + "/AGENTS.md");
        if (pf.good())
            return base;
        base += "/..";
    }
    return ".";
}

inline std::string stripComments(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (i + 1 < s.size() && s[i] == '/' && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n')
                i++;
        } else {
            out += s[i++];
        }
    }
    return out;
}

} // namespace field89
} // namespace autolink

#endif // !ARDUINO
