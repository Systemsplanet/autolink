// OTA pure decision core. Host-linkable: no Arduino /
// ESP-IDF deps. The ARDUINO-side handlers in
// AutoLinkWebHandlers.cpp own the I/O (httpd_req_recv,
// esp_ota_*, LittleFS); everything decidable without a
// socket or flash lives here so the host suite can pin
// it. OtaCoreTest covers the truth tables and the
// split-feed zip boundaries.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace autolink {

// Free-heap margin the OTA receive path must not eat.
constexpr size_t OTA_HEAP_RESERVE = 16384;

// Inactive app slot for a firmware write: round-robin
// past the running slot. -1 when there is no second
// slot (single-app partition table). Mirrors the
// esp_ota_get_next_update_partition policy the device
// path delegates to.
inline int otaInactiveSlot(int running, int count) {
    if (count < 2 || running < 0 || running >= count)
        return -1;
    return (running + 1) % count;
}

// Receive-chunk buffer size for the OTA body drain,
// derived from free heap above the reserve. Clamped
// [1 KB, 8 KB]; heap 0 = unknown (host stub) -> 4 KB.
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

// Sanitize a zip entry name into a LittleFS path under
// /web/. Rejects traversal ("..", absolute, backslash,
// empty) and directory entries (trailing '/'). Returns
// false on reject; on success writes "/web/<name>".
bool otaSafeEntryName(const char *name, char *out, size_t outLen);

// Streaming STORED-only zip parser. feed() accepts the
// body in arbitrary chunk sizes (httpd_req_recv gives no
// alignment guarantees) and drives the callbacks. The
// central directory ends the archive. Compressed
// (method != 0) and data-descriptor (flag bit 3) entries
// are rejected -- sizes must be known up front to stream
// without buffering. Zip the dashboard with `zip -0`.
struct OtaZipCbs {
    void *ctx;
    // false from entry() skips this entry's data;
    // false from data() aborts the parse.
    bool (*entry)(void *ctx, const char *name, uint32_t size);
    bool (*data)(void *ctx, const uint8_t *b, size_t n);
    bool (*entryEnd)(void *ctx);
};

class OtaZipStream {
public:
    explicit OtaZipStream(const OtaZipCbs &cb) : cb_(cb) {}

    // false -> parse error or callback abort; err() says why.
    bool feed(const uint8_t *b, size_t n);
    // true once the central directory was reached.
    bool done() const { return st_ == St::DONE; }
    const char *err() const { return err_; }

    // kNameMax: entry-name byte cap. Renamed from NAME_MAX because
    // <limits.h> (pulled in by FreeRTOS portmacro.h on Arduino)
    // preprocessor-defines NAME_MAX as a numeric macro, which would
    // shadow a constexpr here and break the build with
    // "expected unqualified-id before numeric constant".
    static constexpr size_t kNameMax = 96;

private:
    enum class St : uint8_t { SIG, HDR, NAME, EXTRA, DATA, SKIP, DONE, ERR };
    bool fail(const char *e) {
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
