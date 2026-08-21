// AL89 pin 10 / PongRecvCapReordersTest. Extracted from
// FieldWedgeFixes89Test.cpp (AL90-17 split
// the monolithic 22.7 KB file into one .cpp
// per pin to keep each under the 15 KB cap,
// AGENTS.md rule 20a). The pin's logic is
// unchanged; only the file boundary and the
// function name (per AL90-15) move.
#include "FieldWedgeFixes89Common.h"

using namespace autolink;
using namespace autolink::field89;


// Pin 10 (AL89-10): Pong's recv loop
// is cap-first, recv-second. Toggle
// off (recv-first, cap-second) -> red.
void test_PongRecvCapReordersTest() {
    std::cout << "\n=== Pin 10: Pong recv cap-first, recv-second ==="
              << std::endl;
    std::string src = readFile(projectRoot() + "/src/al/pingpong/Pong.h");
    assert(!src.empty());
    std::string code = stripComments(src);
    // The new shape:
    //   while (recvThisLoop < maxAck &&
    //          (n = base_.comm_.recv(...)) > 0)
    // The old shape:
    //   while ((n = base_.comm_.recv(...)) > 0 &&
    //          recvThisLoop < maxAck)
    assert(code.find("while (recvThisLoop < maxAck &&") != std::string::npos &&
           "Pong's recv loop is not cap-first — the cap is "
           "still tested AFTER recv(). The old order "
           "consumed a message before the cap fired; when "
           "recvThisLoop == maxAck the message was delivered, "
           "drained from the app buf, and thrown away without "
           "an ack. AL89-10's fix is to test the cap first.");
    assert(code.find("while ((n = base_.comm_.recv(base_.buf_") ==
               std::string::npos &&
           "Pong's recv loop is still using the recv-first "
           "shape — the AL89-10 cap-first reorder is gone.");
    std::cout << "  PASS (cap-first, recv-second)" << std::endl;
}

// Pin 11 (AL89-11): Log ring is sized
