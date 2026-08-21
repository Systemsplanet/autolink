
#pragma once
#include "al/util/log/Log.h"
#include <stddef.h>
#include <stdint.h>

namespace autolink {

constexpr size_t OTA_HEAP_RESERVE = 16384;

inline int otaInactiveSlot(int running, int count) {
    if (count < 2 || running < 0 || running >= count)
        return -1;
    return (running + 1) % count;
}

inline size_t otaRecvChunkBytes(size_t freeHeap, size_t reserve) {
    if (freeHeap == 0)
        return 4096;
    size_t avail = freeHeap > reserve ? freeHeap - reserve : 0;
    size_t n = avail / 4;
    if (n < 1024)
        n = 1024;
    if (n > 8192)
        n = 8192;
    return n;
}

bool otaSafeEntryName(const char *name, char *out, size_t outLen);

struct OtaZipCbs {
    void *ctx;

    bool (*entry)(void *ctx, const char *name, uint32_t size);
    bool (*data)(void *ctx, const uint8_t *b, size_t n);
    bool (*entryEnd)(void *ctx);
};

class OtaZipStream {
public:
    explicit OtaZipStream(const OtaZipCbs &cb) : cb_(cb) {}

    bool feed(const uint8_t *b, size_t n);

    bool done() const { return st_ == St::DONE; }
    const char *err() const { return err_; }

    static constexpr size_t kNameMax = 96;

private:
    enum class St : uint8_t { SIG, HDR, NAME, EXTRA, DATA, SKIP, DONE, ERR };
    bool fail(const char *e) {
        // Single point of failure logging for the zip
        // streamer — every call site above is a return on
        // this helper, so the operator sees exactly one log
        // line per aborted upload (no spam from the call
        // site + the helper). Error level: aborted OTA is
        // operator-actionable (rebuild, re-zip with -0).
        Log::log().error("OtaCore", "OTA zip abort: %s", e);
        st_ = St::ERR;
        err_ = e;
        return false;
    }
    OtaZipCbs cb_;
    St st_ = St::SIG;
    const char *err_ = nullptr;
    uint8_t hdr_[26] = {};
    char name_[kNameMax + 1] = {};
    size_t need_ = 0, got_ = 0;
    uint32_t dataLeft_ = 0;
    uint16_t nameLen_ = 0, extraLen_ = 0;
    bool deliver_ = false;
};

} // namespace autolink
