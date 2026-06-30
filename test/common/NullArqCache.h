// No-op IArqCache for tests that don't
// exercise the ARQ cache path. The Link
// ctor takes IArqCache&, so every test
// that constructs a Link must hand it
// some cache. Tests that don't care
// about retransmit-driven cache
// behaviour pass a function-local
// `NullArqCache cache;` and forget about
// it.
//
// The class is intentionally tiny: every
// method is a one-liner. The tests that
// actually drive ARQ use the production
// ArqCache (or a TestCache stub derived
// from ArqCache) — see LinkMessageTest.
#pragma once
#ifndef AUTOLINK_HOST_TEST
#    error "Build with -DAUTOLINK_HOST_TEST (see test/test_desktop/Makefile)"
#endif

#include "al/link/arq/IArqCache.h"

namespace autolink {

class NullArqCache : public IArqCache {
public:
    bool hasRoom() const override { return true; }
    void insert(uint8_t, const uint8_t *, int) override {}
    void freeBySeq(uint8_t) override {}
    bool peekForRetx(uint8_t, const uint8_t **, int *) const override {
        return false;
    }
    void clearAll() override {}
    bool slotInUse(uint8_t) const override { return false; }
    int size() const override { return 0; }
};

} // namespace autolink