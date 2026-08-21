// Aggregator main: runs all 12 AL89
// field-wedge pins and prints a single
// PASS line. The per-pin files are
// split (AL90-17, 15 KB cap rule 20a),
// but the orchestrator runs them as one
// binary so the existing
// `run_test_field_wedge_fixes_89` target
// still works in CI.
//
// Pin 3 (UartEvTaskPinnedToProtocolCoreTest) moved out (AL-D1): it
// needs ARDUINO defined and AUTOLINK_HOST_TEST undefined to actually
// link and run EspHal.cpp for real — the opposite macro shape every
// other pin here needs (AUTOLINK_HOST_TEST for the LinkTestAccessor
// friend class). It's now its own standalone binary,
// run_test_uart_ev_task_pinned_to_protocol_core — see that file.
// Pin 12 (PongScratchHoistedTest) moved out for the same reason
// (Pong.h is ARDUINO-only) — see run_test_pong_scratch_hoisted.
#include "FieldWedgeFixes89Common.h"

using namespace autolink;

void test_SyncMultiChunkDrainRemovedTest();
void test_SyncFloorHasAckHeadroomTest();
void test_ArqWindowClampedToReceiverTest();
void test_HoldNakSelfDescribingTest();
void test_ResendDedupeFloorAndPeerBlockedTest();
void test_BreakWindowEpochTest();
void test_P3SlaveCampBudgetCapTest();
void test_FirstLockAdmissionEvidenceGateTest();
void test_PongRecvCapReordersTest();
void test_LogDropsSurfaceTest();

int main() {
    Log::log().setLevel(Log::NONE);
    test_SyncMultiChunkDrainRemovedTest();
    test_SyncFloorHasAckHeadroomTest();
    test_ArqWindowClampedToReceiverTest();
    test_HoldNakSelfDescribingTest();
    test_ResendDedupeFloorAndPeerBlockedTest();
    test_BreakWindowEpochTest();
    test_P3SlaveCampBudgetCapTest();
    test_FirstLockAdmissionEvidenceGateTest();
    test_PongRecvCapReordersTest();
    test_LogDropsSurfaceTest();
    std::cout << "\nFieldWedgeFixes89Test: all 10 pins passed (pins 3 "
                 "and 12 are now their own binaries — "
                 "run_test_uart_ev_task_pinned_to_protocol_core, "
                 "run_test_pong_scratch_hoisted)"
              << std::endl;
    return 0;
}
