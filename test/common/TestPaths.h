// Resolve a repo-relative path without depending on the
// caller's working directory.
//
// Source-grep pins that hardcode "../../src/..." are green
// under `make` (which runs from test/test_desktop) and red
// when the same binary is run from anywhere else — the
// assert fires on a null FILE*, which reads as a behavioural
// failure rather than a path failure. Walking up for
// AGENTS.md makes the pin say what it means regardless of
// CWD.
//
// (These pins remain a tracked anti-pattern — see
// docs/todo.md. This header only removes their CWD
// dependence; it does not make grepping the source a good
// way to pin behaviour.)
#pragma once

#include <fstream>
#include <string>

namespace autolink {

inline std::string testRepoRoot() {
    std::string base = ".";
    for (int i = 0; i < 8; i++) {
        std::ifstream marker(base + "/AGENTS.md");
        if (marker.good())
            return base;
        base += "/..";
    }
    return std::string(".");
}

inline std::string testRepoPath(const char *rel) {
    return testRepoRoot() + "/" + rel;
}

} // namespace autolink
