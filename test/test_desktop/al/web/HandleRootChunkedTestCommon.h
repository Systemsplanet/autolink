// Shared helpers for the HandleRootChunkedTest split.
//
// The original HandleRootChunkedTest.cpp grew to ~700
// lines covering four orthogonal concerns:
//   * handleRoot's chunked-send contract
//   * begin() lifecycle (log sink, version line, httpd wait)
//   * httpd retry budget (TIME_WAIT settle + WiFi retry-forever)
//   * Link::begin() deferred kickoff when paused
//
// Splitting it across HandleRootChunkedTest /
// WebBeginLifecycleTest / WebHttpdRetryTest /
// LinkBeginDeferTest keeps each TU focused. This
// header holds the brace-balanced source-slice helpers
// that all four TUs share (readFile / projectRoot /
// extractHandleRootBody / extractFnBody /
// extractHttpConfigBlock).
#pragma once
#ifndef ARDUINO

#    include <cassert>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>
#    include "al/util/Log.h"

namespace {

std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.good())
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

// Extracts the body of `AutoLinkWeb::handleRoot` from
// AutoLinkWeb.cpp so the assertions stay scoped to that
// function (a stray `httpd_resp_send` in another handler
// must not satisfy this gate). Walks back from the function
// header to find the opening brace, then forward to the
// matching close.
std::string extractHandleRootBody(const std::string &src) {
    auto headerPos = src.find("AutoLinkWeb::handleRoot(httpd_req_t *req)");
    if (headerPos == std::string::npos)
        return "";
    auto braceOpen = src.find('{', headerPos);
    if (braceOpen == std::string::npos)
        return "";
    int depth = 1;
    std::size_t i = braceOpen + 1;
    for (; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0)
                return src.substr(braceOpen, i + 1 - braceOpen);
        }
    }
    return "";
}

// Scoped to the httpd config block in setupHttpAndLogging_:
// walks back from `cfg.lru_purge_enable` to its enclosing
// brace-balanced block (the arm that initialises
// `httpd_config_t cfg = HTTPD_DEFAULT_CONFIG(); ...`).
// Same shape as UriHandlerAlignmentTest::extractEnclosingBracedBlock.
// Walk from a function signature (e.g. "void setup()")
// to its matching closing brace and return the body
// in between (signature line inclusive).
std::string extractFnBody(const std::string &src, const std::string &sig) {
    auto sigPos = src.find(sig);
    if (sigPos == std::string::npos)
        return "";
    auto openBrace = src.find('{', sigPos);
    if (openBrace == std::string::npos)
        return "";
    int depth = 0;
    for (std::size_t i = openBrace; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0)
                return src.substr(sigPos, i - sigPos + 1);
        }
    }
    return "";
}

std::string extractHttpConfigBlock(const std::string &src) {
    auto anchor = src.find("cfg.lru_purge_enable");
    if (anchor == std::string::npos)
        return "";
    int back = 0;
    std::size_t openPos = std::string::npos;
    for (std::size_t i = anchor; i > 0; i--) {
        if (src[i] == '}') {
            back++;
        } else if (src[i] == '{') {
            if (back == 0) {
                openPos = i;
                break;
            }
            back--;
        }
    }
    if (openPos == std::string::npos)
        return "";
    int depth = 1;
    std::size_t closePos = std::string::npos;
    for (std::size_t i = openPos + 1; i < src.size(); i++) {
        if (src[i] == '{')
            depth++;
        else if (src[i] == '}') {
            depth--;
            if (depth == 0) {
                closePos = i;
                break;
            }
        }
    }
    if (closePos == std::string::npos)
        return "";
    return src.substr(openPos, closePos + 1 - openPos);
}

} // namespace

#endif
