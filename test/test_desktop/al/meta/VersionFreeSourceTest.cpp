#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include <fstream>
#    include <regex>
#    include <string>
#    include <vector>

// Pins AGENTS.md rules 11 and 12 in one walk.
// Rule 12: no .cpp / .h / .ino / .py / .sh /
// .mk / .yml / .json / .txt / non-Version .md
// file hard-codes a X.Y.Z version string except
// the AUTOLINK_VERSION macro in include/AutoLink.h
// (the rule's only legitimate exception). Also
// includes extension-less Makefile basenames so
// the per-suite build-rule comments (which are
// the natural place to put a regression pin) are
// covered too.
// Rule 11: no comment in any of those files
// carries a historical anchor — "todo item N",
// "fix plan", or "pre-fix". A reader needs to
// know what a pin protects and what the
// toggle-off shape is, not which release or
// which todo item fixed it. The rationale
// belongs in docs/Version.md.
// Fails the build, not just a test, if a version
// reference or a comment anchor slips in:
// re-introducing either becomes a compile-time
// regression.

static const std::vector<std::string> kSourceExts = {
    ".cpp", ".h", ".ino", ".py", ".sh", ".mk", ".yml", ".json", ".txt", ".md",
};

static const std::vector<std::string> kSourceNames = {
    "Makefile",
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
    if (path.find("todo.md") != std::string::npos)
        return true;
    if (path.find("verify_build/verify_build.ino") != std::string::npos)
        return true;
    if (path.find("VersionFreeSourceTest.cpp") != std::string::npos)
        return true;
    return false;
}

// Walk the source tree. The find command combines
// the extension list + the basename list (Makefile)
// in one pass. kSourceNames covers the
// extension-less files; kSourceExts covers
// everything else.
static std::string buildFindCmd(const std::string &root) {
    std::string cmd = "find " + root + " -type f \\( ";
    bool first = true;
    for (const auto &e : kSourceExts) {
        if (!first)
            cmd += " -o ";
        cmd += "-name '*";
        cmd += e;
        cmd += "'";
        first = false;
    }
    for (const auto &n : kSourceNames) {
        if (!first)
            cmd += " -o ";
        cmd += "-name '";
        cmd += n;
        cmd += "'";
        first = false;
    }
    cmd += " \\)";
    return cmd;
}

// Match a project version (v?MAJOR.Y.Z, majors 5-9) with
// non-letter, non-digit boundaries so IPs and
// third-party pins (127.0.0.1, esp32@3.3.5) don't trip.
// Skips lines referencing AUTOLINK_VERSION (the macro is
// the only legitimate version token). The separator
// accepts both `.` (a real version literal) and `_`
// (underscore-joined forms like `v5_1_54` — a test
// function name that bypassed the prior dot-only regex),
// and the third component accepts `[0-9x]+` so a literal
// `x` in the patch slot (e.g. `5.3.x` in a comment
// referring to "any 5.3.x release") is also caught. The
// boundary allows `_` and `.` as separators (not letters
// or digits), so a token like `test_thing_v5_1_54_helper`
// matches — the `_` immediately before `v5` is the start
// of a new sub-token. Both underscore-joined and
// `x`-suffixed forms appeared in the tree before this
// fix; the dot-only regex matched neither. Toggle-
// verified: inserting `v5_1_54` and `5.3.x` in test
// sources flips this test red; removing them returns to
// green.
static const std::regex
    kVersionPattern(R"((?:^|[^A-Za-z0-9.])(v?[5-9][_.][0-9]+[_.][0-9x]+))");

// Rule 11: comment archaeology. AGENTS rule 11 says no
// historical anchors in code or comments. Phrases banned:
// "todo item N" (numeric back-reference), "fix plan" / "the
// campaign" (release-narrative prose), "pre-fix" / "post-fix"
// / "the old shape" / "the fix shape" / "broken shape" /
// "pre-this-<word>" / "used to" / "" (contrastive
// "the broken shape" / "this used to do X" anchors — these
// all narrate a before/after across a specific past change),
// and "the bench log" / "long-haul test run(s)" (a reference
// to a specific, non-reproducible past debug session). All
// are case-insensitive — "Pre-fix" is exactly as much an
// anchor as "pre-fix". Each pattern carries a word boundary
// so "prefixed" / "unfixable" do not trip. Deliberately
// excluded: bare "previously" — "a previously-OK link"
// describes a test's own timeline, not a past release, and a
// blanket ban produces exactly the false positive rule 11
// warns against forcing (rewriting away a legitimate
// timeless description). Applied per-line to a comment
// region; the caller is responsible for passing only comment
// text.
static const auto kIC = std::regex::icase;
static const std::vector<std::regex> kCommentAnchorPatterns = {
    std::regex(R"(\btodo item\s+[0-9]+)", kIC),
    std::regex(R"(\bfix plan\b)", kIC),
    std::regex(R"(\bthe campaign\b)", kIC),
    std::regex(R"(\bpre-fix\b)", kIC),
    std::regex(R"(\bpost-fix\b)", kIC),
    std::regex(R"(\bpre-this-\w+\b)", kIC),
    std::regex(R"(\bthe old shape\b)", kIC),
    std::regex(R"(\bthe fix shape\b)", kIC),
    std::regex(R"(\bbroken shape\b)", kIC),
    std::regex(R"(\bused to\b)", kIC),
    std::regex(R"(\bthe broken shape\b)", kIC),
    std::regex(R"(\bthe bench log\b)", kIC),
    std::regex(R"(\blong-haul test runs?\b)", kIC),
};

void test_no_hardcoded_version_in_sources() {
    std::cout << "\n=== Test: rule-9 no hard-coded version in source ==="
              << std::endl;
    // A single root ("../.." = the project root from
    // test/test_desktop) covers src/, include/, build/, test/, and
    // examples/ — no need to walk overlapping sub-roots, which would
    // double-report every violation under two matching roots.
    std::string cmd = buildFindCmd("../..");
    std::vector<std::string> violations;
    FILE *p = popen(cmd.c_str(), "r");
    if (p) {
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
            // Filename check: a test or example named
            // v6_1_51_FooTest.cpp encodes the version
            // outside of any file body, and would not
            // be caught by the per-line scan. AGENTS
            // rule 12 (no version references in code
            // or scripts) applies to the filename too.
            if (std::regex_search(path, kVersionPattern)) {
                std::string m = path + ":0: (filename)";
                violations.push_back(m);
            }
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
            std::cerr << " " << v << std::endl;
        assert(violations.empty());
    }
    std::cout << "PASS" << std::endl;
}

// Find the start of a // line comment, returning the index
// just after the //, or std::string::npos if the line has
// none. Skips // inside string literals by walking char by
// char, tracking quote state. The walk is a lightweight
// approximation — it does not handle trigraphs, raw
// strings, or line continuations. Adequate for the source
// files this suite covers.
static size_t findLineCommentStart(const std::string &line) {
    bool inStr = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '\\' && inStr) {
            i++;
            continue;
        }
        if (c == '"')
            inStr = !inStr;
        else if (!inStr && c == '/' && i + 1 < line.size() &&
                 line[i + 1] == '/')
            return i + 2;
    }
    return std::string::npos;
}

// Find the start of a # line comment (used by Makefile, Python,
// shell, YAML, and Markdown), returning the index just after the
// #, or std::string::npos if the line has none. Skips # inside
// string literals.
static size_t findHashCommentStart(const std::string &line) {
    bool inStr = false;
    for (size_t i = 0; i < line.size(); i++) {
        char c = line[i];
        if (c == '\\' && inStr) {
            i++;
            continue;
        }
        if (c == '"' || c == '\'')
            inStr = !inStr;
        else if (!inStr && c == '#')
            return i + 1;
    }
    return std::string::npos;
}

// Returns true if the file uses C++ comment syntax (// and /* */
// for line and block comments), false if it uses # for line
// comments (Makefile, Python, shell, YAML, Markdown). The block-
// comment walker is only used for C++ files; # files have no
// block-comment syntax.
static bool isCppCommentFile(const std::string &path) {
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".cpp") == 0)
        return true;
    if (path.size() >= 2 && path.compare(path.size() - 2, 2, ".h") == 0)
        return true;
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".ino") == 0)
        return true;
    return false;
}

void test_no_comment_anchors_in_sources() {
    std::cout << "\n=== Test: rule-11 no comment archaeology anchors ==="
              << std::endl;
    // "../.." (the project root) covers src/, include/, build/,
    // test/, and examples/ in one walk — see the note on the rule-9
    // root list above.
    std::string cmd = buildFindCmd("../..");
    std::vector<std::string> violations;
    FILE *p = popen(cmd.c_str(), "r");
    if (p) {
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
            const bool cppStyle = isCppCommentFile(path);
            while (std::getline(in, line)) {
                lineno++;
                // For # files: walk the # line-comment syntax. The
                // line is a comment from findHashCommentStart to EOL.
                if (!cppStyle) {
                    size_t hstart = findHashCommentStart(line);
                    if (hstart != std::string::npos) {
                        std::string comment = line.substr(hstart);
                        for (const auto &pat : kCommentAnchorPatterns) {
                            if (std::regex_search(comment, pat)) {
                                std::string m = path + ":" +
                                    std::to_string(lineno) + ": " + line;
                                violations.push_back(m);
                                break;
                            }
                        }
                    }
                    continue;
                }
                // C++ style. Look for /* block start or // line start.
                size_t blockStart = line.find("/*");
                size_t cstart = findLineCommentStart(line);
                if (blockStart != std::string::npos &&
                    (cstart == std::string::npos || blockStart < cstart - 2)) {
                    // Block comment on this line. Find the matching */.
                    size_t blockEnd = line.find("*/", blockStart + 2);
                    std::string slice = (blockEnd == std::string::npos)
                        ? line.substr(blockStart)
                        : line.substr(blockStart, blockEnd + 2 - blockStart);
                    for (const auto &pat : kCommentAnchorPatterns) {
                        if (std::regex_search(slice, pat)) {
                            std::string m = path + ":" +
                                std::to_string(lineno) + ": " + line;
                            violations.push_back(m);
                            break;
                        }
                    }
                } else if (cstart != std::string::npos) {
                    std::string comment = line.substr(cstart);
                    for (const auto &pat : kCommentAnchorPatterns) {
                        if (std::regex_search(comment, pat)) {
                            std::string m = path + ":" +
                                std::to_string(lineno) + ": " + line;
                            violations.push_back(m);
                            break;
                        }
                    }
                }
            }
        }
        pclose(p);
    }
    if (!violations.empty()) {
        std::cerr << "FAIL: comment archaeology anchors found:" << std::endl;
        for (const auto &v : violations)
            std::cerr << " " << v << std::endl;
        assert(violations.empty());
    }
    std::cout << "PASS" << std::endl;
}

int main() {
    std::cout << "=== Running Version-Free Source Tests ===" << std::endl;
    test_no_hardcoded_version_in_sources();
    test_no_comment_anchors_in_sources();
    std::cout << "\n=== Version-Free Source Tests Completed ===" << std::endl;
    return 0;
}

#endif
