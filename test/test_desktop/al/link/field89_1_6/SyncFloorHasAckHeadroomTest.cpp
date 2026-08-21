// AL89 pin 2 / SyncFloorHasAckHeadroomTest. Extracted from
// FieldWedgeFixes89Test.cpp (AL90-17 split
// the monolithic 22.7 KB file into one .cpp
// per pin to keep each under the 15 KB cap,
// AGENTS.md rule 20a). The pin's logic is
// unchanged; only the file boundary and the
// function name (per AL90-15) move.
#include "FieldWedgeFixes89Common.h"

using namespace autolink;
using namespace autolink::field89;

// branch sizes to msgChunks + 2 (the +1
// of headroom for a concurrent outbound
// ACK in the ring). Toggle off (revert
// to msgChunks + 1) -> red.
void test_SyncFloorHasAckHeadroomTest() {
    std::cout << "\n=== Pin 2: SYNC floor sized to msgChunks + 2 ==="
              << std::endl;
    AutoLinkConfig cfg;
    cfg.mode = AutoLinkConfig::Mode::SYNC;
    cfg.maxMsg = 2048;
    size_t floor = uartTxBufferFloor(cfg);
    int msgChunks = chunksForMsgLen((int)cfg.maxMsg);
    size_t expected = (size_t)kWorstCaseCobsFrame * (size_t)(msgChunks + 2);
    assert(floor == expected &&
           "uartTxBufferFloor's SYNC branch does not size to "
           "msgChunks + 2 — the +1 of ACK headroom is gone. A "
           "fully-loaded ring that just accepted an in-flight "
           "chunk has no room for the peer ACK it has to send in "
           "the same window.");
    std::cout << "  PASS (floor=" << floor
              << " = kWorstCaseCobsFrame*(msgChunks+2))" << std::endl;
}

// Pin 3 (AL89-3): uart_event_task pinned
