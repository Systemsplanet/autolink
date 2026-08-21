// AL89 pin 8 / P3SlaveCampBudgetCapTest. Extracted from
// FieldWedgeFixes89Test.cpp (AL90-17 split
// the monolithic 22.7 KB file into one .cpp
// per pin to keep each under the 15 KB cap,
// AGENTS.md rule 20a). The pin's logic is
// unchanged; only the file boundary and the
// function name (per AL90-15) move.
//
// AL-D1: a real behavioral conversion was attempted twice this
// project (once as a standalone P3CampBudgetTest.cpp addition,
// once here) and abandoned both times — a scratch revert of the
// deadlineElapsed check under test produced NO observable change
// in a driven camp's exit timing in either harness, meaning the
// harness was reaching P3 exit via some other mechanism entirely
// (the slave's initial P3-entry timer, armed directly in
// LinkSweep::enterPhase3, appears to dominate before this
// re-arm's deadlineElapsed check is ever reached) rather than
// exercising the code this pin claims to test. Shipping a test
// that passes regardless of the mechanism under test is worse
// than the honest source-grep below — it would look like coverage
// while providing none. Left as a source-grep pending a harness
// that can actually reach and discriminate this specific re-arm
// branch.
#include "FieldWedgeFixes89Common.h"

using namespace autolink;
using namespace autolink::field89;

// when the deadline has elapsed,
// regardless of the attempt count.
// Toggle off (re-arm count alone) ->
// red.
void test_P3SlaveCampBudgetCapTest() {
    std::cout << "\n=== Pin 8: P3 slave camp re-arm is bounded by "
                 "the wall-clock budget ==="
              << std::endl;
    std::string src =
        readFile(projectRoot() + "/src/al/link/timers/LinkTimersSwp.cpp");
    assert(!src.empty());
    std::string code = stripComments(src);
    // The new shape checks
    // `resweepPrefDeadlineMs_ != 0 &&
    //  (now - resweepPrefDeadlineMs_) >= 0`
    // and falls to P1 unconditionally
    // when the deadline has elapsed.
    assert(code.find("deadlineElapsed") != std::string::npos &&
           "deadlineElapsed is missing from the P3 slave "
           "camp re-arm branch — the wall-clock budget cap "
           "is gone. The field capture's slave camped 10.7 s "
           "(two re-arms × 5 s) while the master was walking "
           "P1 from a baud-mismatch escalation; the attempt "
           "count alone let the re-arm dwell double the "
           "budget before the ceiling fired.");
    std::cout << "  PASS (deadlineElapsed cap present)" << std::endl;
}
