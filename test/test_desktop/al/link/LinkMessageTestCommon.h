// Shared harness for the LinkMessageTest split.
//
// The original LinkMessageTest.cpp grew to ~1000 lines
// covering five orthogonal concerns:
//   * round-trip framing (size sweep, back-to-back)
//   * corruption detection (CRC, payload bit-flips)
//   * resync from a corrupted MSG_HDR
//   * edge cases (zero-byte send, recv buffer too small)
//   * shared test cache stub (TestCache)
//
// Splitting it across LinkMessageRoundtripTest /
// LinkMessageCorruptTest / LinkMessageResyncTest /
// LinkMessageEdgeTest removes the scrolling cost but
// keeps the harness in one place. Each TU #includes
// this header to pick up the TestCache stub, the
// `test_internal` namespace, and the shared includes.
#pragma once
#ifndef ARDUINO

#    include <cstdint>
#    include "al/link/arq/ArqCache.h"
namespace test_internal {
// Stub ArqCache for the chunk-boundary
// test. The production cache stores
// actual bytes; this stub only counts
// chunks in flight so hasRoom() can
// fail at a custom boundary (CAP=240).
class TestCache : public autolink::ArqCache {
public:
    int count = 0;
    static constexpr int CAP = 240;

    bool hasRoom() const override { return count < CAP; }
    void insert(uint8_t, const uint8_t *, int) override {
        // Chunk accounting: real cache
        // stores bytes; the stub just
        // counts each insert as one
        // chunk. (Production caller
        // passes chunkCount=1 from the
        // short-msg path, which the
        // chunking path does not hit
        // here — this test only sends
        // short msgs.)
        count++;
    }
    void freeBySeq(uint8_t seq) override {
        if (count > 0)
            count--;
        autolink::ArqCache::freeBySeq(seq);
    }
    void clearAll() override {
        count = 0;
        autolink::ArqCache::clearAll();
    }
};
} // namespace test_internal
#    include <iostream>
#    include <cassert>
#    include <vector>
#    include <cstdlib>
#    include "al/util/Log.h"
#    include "AutoLink.h"
#    include "MockHal.h"
#    include "NullArqCache.h"

#endif
