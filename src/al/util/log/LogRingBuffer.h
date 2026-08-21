#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace autolink {

// Fixed-capacity, no-heap-allocation ring buffer for one log line.
// N must be a compile-time constant; entries are plain structs
// (POD-ish, fixed-size char arrays) so there is no dynamic
// allocation on push — a burst of verbose logging must not
// fragment the heap on-device.
//
// This class does NOT lock. It is meant to be used from exactly
// one context that already holds whatever critical section fits
// the platform (a FreeRTOS spinlock on ESP32, nothing needed on a
// single-threaded host test) around each tryPush()/tryPop() call.
// Kept separate from any RTOS header so the wraparound and
// drop-oldest logic is unit-testable on host. Pinned by
// LogRingBufferTest.
template<size_t N> class LogRingBuffer {
public:
    struct Entry {
        char sev = 'I';
        char tag[16] = { 0 };
        // AL90-6 / AL94-1: msg shrunk from 400
        // to 160; QUEUE_CAP shrunk from 256 to
        // 128 (see Log.h). The longest line in
        // the field capture (excluding the
        // dashboard HTML never logged) is 96
        // chars; 160 gives 64 chars of headroom
        // for the timestamp + severity prefix
        // an operator reads at runtime. This
        // ring is a function-local static in
        // Log::log(), charged to .bss before
        // setup() runs; a real cross-compile
        // against the field-tested target
        // failed to link with the 400 B/×257
        // and, later, the 160 B/×257 (47 KB)
        // shapes — the DRAM segment budget is
        // shared statically by the whole
        // firmware image, not just what's left
        // of free heap at runtime. 160 B×129 =
        // ~23 KB is the current shape; see
        // Log.h for the measured overflow this
        // was calibrated against. The msg[]
        // size and QUEUE_CAP are both
        // compile-time constants so a future
        // bump fails the static_assert in
        // Log.h before booting. Pinned by
        // LogEntrySizeBudgetTest.
        char msg[160] = { 0 };
        // Captured at push time (Log::emit()), not at drain time.
        // ESP_LOGx's own (nnnnn) prefix is stamped when the drain task
        // actually calls it, which can be far later than when the
        // producer logged the event if the queue backs up — every
        // interval an operator reads off consecutive lines is then
        // fiction. Pinned by LogTimestampCaptureTest.
        uint32_t tsMs = 0;
    };

    // Always succeeds: if full, drops the oldest entry to make
    // room (drop-oldest, so the most recent state is preserved on
    // overflow — an operator wants to see current state, not a
    // stale backlog). Returns true if an existing entry was
    // dropped to make room.
    bool tryPush(char sev, const char *tag, const char *msg,
                 uint32_t tsMs = 0) {
        size_t next = (tail_ + 1) % N;
        bool dropped = false;
        if (next == head_) {
            head_ = (head_ + 1) % N;
            dropped = true;
        }
        Entry &e = ring_[tail_];
        e.sev = sev;
        std::strncpy(e.tag, tag, sizeof(e.tag) - 1);
        e.tag[sizeof(e.tag) - 1] = 0;
        std::strncpy(e.msg, msg, sizeof(e.msg) - 1);
        e.msg[sizeof(e.msg) - 1] = 0;
        e.tsMs = tsMs;
        tail_ = next;
        return dropped;
    }

    // False if empty; otherwise copies the oldest entry out and
    // advances head_.
    bool tryPop(Entry *out) {
        if (head_ == tail_)
            return false;
        *out = ring_[head_];
        head_ = (head_ + 1) % N;
        return true;
    }

    bool empty() const { return head_ == tail_; }
    size_t capacity() const { return N; }

    // Number of entries currently queued. O(1), safe to call
    // without a lock for diagnostics (may be stale by one entry
    // under concurrent access, which is fine for a size hint).
    size_t size() const { return (tail_ + N - head_) % N; }

private:
    Entry ring_[N];
    size_t head_ = 0;
    size_t tail_ = 0;
};

} // namespace autolink
