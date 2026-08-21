// Pinned regression: the fixed-capacity ring buffer that backs
// the ESP32 log queue (Log::emit() -> tryPush(), the background
// drain task -> tryPop()) must never allocate on push, must
// preserve FIFO order, and must drop the OLDEST entry on overflow
// (not the newest, and not silently corrupt state) so the most
// recent link state is what survives a burst.
//
//   Pin 1: push/pop preserves FIFO order for a normal (non-full)
//          sequence.
//   Pin 2: a template capacity of N holds N-1 usable entries (one
//          slot reserved to distinguish full from empty) — this
//          is the load-bearing invariant Log.cpp's capacity
//          constant must account for.
//   Pin 3: pushing past capacity drops the OLDEST entry, not the
//          newest — the queue always holds the most recent state.
//   Pin 4: tryPush reports whether it dropped an entry, so the
//          caller can drive a one-shot overflow warning.
//   Pin 5: severity, tag, and message all round-trip intact
//          (truncated safely if a field exceeds its fixed size,
//          never overruns the fixed buffer).
//   Pin 6: empty()/size() reflect the true queue state through a
//          full push/drain/re-push cycle (wraparound doesn't
//          desync the accounting).
#include <cassert>
#include <cstring>
#include <iostream>
#include "al/util/log/LogRingBuffer.h"

using namespace autolink;

static void test_fifo_order() {
    std::cout << "\n=== Pin 1: FIFO order preserved ===" << std::endl;
    LogRingBuffer<8> rb;
    assert(!rb.tryPush('I', "A", "one"));
    assert(!rb.tryPush('W', "B", "two"));
    assert(!rb.tryPush('E', "C", "three"));

    LogRingBuffer<8>::Entry e;
    assert(rb.tryPop(&e));
    assert(e.sev == 'I' && std::strcmp(e.tag, "A") == 0 &&
           std::strcmp(e.msg, "one") == 0);
    assert(rb.tryPop(&e));
    assert(e.sev == 'W' && std::strcmp(e.msg, "two") == 0);
    assert(rb.tryPop(&e));
    assert(e.sev == 'E' && std::strcmp(e.msg, "three") == 0);
    assert(!rb.tryPop(&e) &&
           "Pin 1: queue must be empty after 3 pops "
           "following 3 pushes");
    std::cout << "  PASS (push A,B,C -> pop returns A,B,C in order)"
              << std::endl;
}

static void test_usable_capacity_is_n_minus_one() {
    std::cout << "\n=== Pin 2: template capacity N holds N-1 usable "
                 "entries ==="
              << std::endl;
    LogRingBuffer<4> rb;
    // Push exactly N-1 = 3 entries: none should report a drop.
    assert(!rb.tryPush('I', "T", "1"));
    assert(!rb.tryPush('I', "T", "2"));
    assert(!rb.tryPush('I', "T", "3"));
    // The 4th push into a capacity-4 buffer must drop the oldest
    // (head==tail collision at full) — this is the N-1 usable
    // invariant Log.cpp's ESP_QUEUE_CAP must size for.
    assert(rb.tryPush('I', "T", "4") &&
           "Pin 2: the Nth push into a capacity-N ring must report a "
           "drop — only N-1 entries are ever simultaneously held");
    std::cout << "  PASS (3 pushes clean, 4th into capacity-4 buffer "
                 "drops)"
              << std::endl;
}

static void test_overflow_drops_oldest_not_newest() {
    std::cout << "\n=== Pin 3: overflow drops the OLDEST entry, not the "
                 "newest ==="
              << std::endl;
    LogRingBuffer<4> rb;
    rb.tryPush('I', "T", "1");
    rb.tryPush('I', "T", "2");
    rb.tryPush('I', "T", "3");
    // Buffer now full (3 = capacity-1). Pushing "4" must evict "1".
    bool dropped = rb.tryPush('I', "T", "4");
    assert(dropped);

    LogRingBuffer<4>::Entry e;
    assert(rb.tryPop(&e));
    assert(std::strcmp(e.msg, "2") == 0 &&
           "Pin 3: after overflow, the OLDEST surviving entry must be "
           "'2' (entry '1' was dropped) — dropping the newest instead "
           "would mean the queue no longer reflects current state");
    assert(rb.tryPop(&e) && std::strcmp(e.msg, "3") == 0);
    assert(rb.tryPop(&e) && std::strcmp(e.msg, "4") == 0);
    assert(!rb.tryPop(&e));
    std::cout << "  PASS (push 1,2,3,4 into capacity-4 -> drains as "
                 "2,3,4)"
              << std::endl;
}

static void test_push_reports_drop_for_warning_latch() {
    std::cout << "\n=== Pin 4: tryPush return value drives a one-shot "
                 "overflow warning ==="
              << std::endl;
    LogRingBuffer<4> rb;
    assert(!rb.tryPush('I', "T", "1"));
    assert(!rb.tryPush('I', "T", "2"));
    assert(!rb.tryPush('I', "T", "3"));
    bool sawDrop = false;
    for (int i = 0; i < 5; i++) {
        if (rb.tryPush('I', "T", "x"))
            sawDrop = true;
    }
    assert(sawDrop &&
           "Pin 4: sustained overflow must keep reporting "
           "drops so the caller's warning latch can fire");
    std::cout << "  PASS (sustained overflow keeps reporting drops)"
              << std::endl;
}

static void test_fields_round_trip_and_truncate_safely() {
    std::cout << "\n=== Pin 5: sev/tag/msg round-trip; oversized fields "
                 "truncate without overrun ==="
              << std::endl;
    LogRingBuffer<4> rb;
    rb.tryPush('E', "AutoLink", "normal message");
    LogRingBuffer<4>::Entry e;
    assert(rb.tryPop(&e));
    assert(e.sev == 'E');
    assert(std::strcmp(e.tag, "AutoLink") == 0);
    assert(std::strcmp(e.msg, "normal message") == 0);

    // Oversized tag/msg: must truncate to fit the fixed buffers,
    // not overrun them (sizeof(e.tag)=16, sizeof(e.msg)=400 by
    // Entry's own definition).
    std::string longTag(64, 'T');
    std::string longMsg(1000, 'M');
    rb.tryPush('W', longTag.c_str(), longMsg.c_str());
    assert(rb.tryPop(&e));
    assert(e.sev == 'W');
    assert(std::strlen(e.tag) == sizeof(e.tag) - 1 &&
           "Pin 5: an oversized tag must truncate to sizeof(tag)-1, "
           "null-terminated, never overrun the fixed buffer");
    assert(std::strlen(e.msg) == sizeof(e.msg) - 1 &&
           "Pin 5: an oversized msg must truncate to sizeof(msg)-1, "
           "null-terminated, never overrun the fixed buffer");
    std::cout << "  PASS (normal fields round-trip; oversized fields "
                 "truncate safely)"
              << std::endl;
}

static void test_empty_and_size_survive_wraparound() {
    std::cout << "\n=== Pin 6: empty()/size() stay correct through a "
                 "full push/drain/re-push wraparound cycle ==="
              << std::endl;
    LogRingBuffer<4> rb;
    assert(rb.empty());
    assert(rb.size() == 0);
    rb.tryPush('I', "T", "a");
    rb.tryPush('I', "T", "b");
    assert(!rb.empty());
    assert(rb.size() == 2);

    LogRingBuffer<4>::Entry e;
    rb.tryPop(&e);
    assert(rb.size() == 1);

    // Push past the physical end of the backing array to force
    // index wraparound, then drain fully.
    rb.tryPush('I', "T", "c");
    rb.tryPush('I', "T", "d");
    rb.tryPush('I', "T", "e"); // wraps; may drop depending on capacity
    while (rb.tryPop(&e)) {
    }
    assert(rb.empty() &&
           "Pin 6: after fully draining, empty() must be "
           "true regardless of how many wraps occurred");
    assert(rb.size() == 0);
    std::cout << "  PASS (empty/size correct after wraparound + full "
                 "drain)"
              << std::endl;
}

// Pin 7: tsMs round-trips through push/pop, distinct per entry, and
// defaults to 0 when the caller doesn't supply one (backward
// compatible with every other call site in this file). The
// timestamp is the producer's captured call-time value — Log::emit()
// passes esp_log_timestamp() at push, not left for the drain task
// to stamp whenever it happens to get to the entry. Without this
// field, a burst of queued lines all print with whatever timestamp
// ESP_LOGx has live when the drain task finally reaches them, so
// consecutive printed lines can carry near-identical timestamps
// while real elapsed time between the events they describe was
// much larger — every duration read off the field log becomes
// fiction.
static void test_timestamp_round_trips_and_defaults_to_zero() {
    std::cout << "\n=== Pin 7: tsMs round-trips, defaults to 0 ===\n";
    LogRingBuffer<8> rb;
    assert(!rb.tryPush('I', "A", "no ts arg"));
    assert(!rb.tryPush('I', "B", "explicit ts", 12345u));
    assert(!rb.tryPush('I', "C", "later ts", 99999u));

    LogRingBuffer<8>::Entry e;
    assert(rb.tryPop(&e));
    assert(e.tsMs == 0 && "omitted tsMs must default to 0");
    assert(rb.tryPop(&e));
    assert(e.tsMs == 12345u && "explicit tsMs must round-trip exactly");
    assert(rb.tryPop(&e));
    assert(e.tsMs == 99999u &&
           "each entry keeps its OWN captured tsMs — a later push must "
           "not overwrite an earlier, still-queued entry's timestamp");
    std::cout << "  PASS (tsMs round-trips per-entry; defaults to 0)"
              << std::endl;
}

int main() {
    std::cout << "=== Log Ring Buffer Tests ===" << std::endl;
    test_fifo_order();
    test_usable_capacity_is_n_minus_one();
    test_overflow_drops_oldest_not_newest();
    test_push_reports_drop_for_warning_latch();
    test_fields_round_trip_and_truncate_safely();
    test_empty_and_size_survive_wraparound();
    test_timestamp_round_trips_and_defaults_to_zero();
    std::cout << "\n=== All 7 log ring buffer pins PASS ===" << std::endl;
    return 0;
}
