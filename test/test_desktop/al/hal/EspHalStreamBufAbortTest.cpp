// Source-level pin for the EspHal::begin() heap-refusal abort path.
// On xStreamBufferCreate failure, the branch must log, clean up the
// mutex + task_exit_sem, set running=false, and return without
// setting healthy=true — logging and falling through instead leaves
// the link task running with a null stream_buf (every app-buf push
// silently dropped) while bringUpLink's isHealthy() gate reads true,
// because that flag is set at the bottom of begin() after every
// other RTOS primitive passed. The link looks up but every received
// byte is thrown away — a silent wire. `#ifdef ARDUINO`-only, lives
// in EspHal.h (EspHal.cpp doesn't exist; the class is entirely
// inline). Source-grep only — the shape is what matters.
#ifndef ARDUINO

#    include <cassert>
#    include <fstream>
#    include <iostream>
#    include <sstream>
#    include <string>

namespace {

std::string readFile(const std::string &path) {
    std::ifstream f(path);
    if (!f.good())
        return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string projectRoot() {
    std::string base = ".";
    for (int i = 0; i < 8; i++) {
        std::ifstream pf(base + "/AGENTS.md");
        if (pf.good())
            return base;
        base += "/..";
    }
    return ".";
}

void test_esphal_begin_aborts_on_stream_buf_failure() {
    std::cout
        << "\n=== Pin: EspHal::begin() aborts on xStreamBufferCreate failure ==="
        << std::endl;
    // begin()'s body lives in EspHal.cpp (out-of-class definition);
    // EspHal.h only carries the declaration.
    std::string src = readFile(projectRoot() + "/src/al/hal/EspHal.cpp");
    assert(!src.empty());

    // Locate the xStreamBufferCreate call site.
    auto p = src.find("xStreamBufferCreate(stream_buf_size_, 1)");
    assert(p != std::string::npos);

    // When the create returns null, the body must (a) log,
    // (b) clean up mutex + task_exit_sem, (c) set running=false,
    // (d) return.
    std::string slice = src.substr(p, 2500);

    // (a) log
    assert(slice.find("aborting begin()") != std::string::npos &&
           "begin() failure path must log 'aborting begin()'");
    // (b) cleanup
    assert(slice.find("vSemaphoreDelete(mutex)") != std::string::npos &&
           "must delete mutex on stream-buf failure");
    assert(slice.find("vSemaphoreDelete(task_exit_sem)") != std::string::npos &&
           "must delete task_exit_sem on stream-buf failure");
    // (c) running=false
    assert(slice.find("running = false") != std::string::npos &&
           "must set running=false before return");
    // (d) early return (no healthy=true at the bottom
    // for this branch)
    assert(slice.find("return;") != std::string::npos &&
           "must early-return so the bottom healthy=true never fires");

    assert(slice.find("check heap") == std::string::npos &&
           "old 'check heap' message must be replaced");
    std::cout << "  PASS (log, cleanup mutex+task_exit_sem, running=false, "
                 "return)"
              << std::endl;
}

void test_pingpongbase_halts_on_unhealthy() {
    std::cout << "\n=== Pin: PingPongBase::bringUpLink halts on "
                 "!comm.isHealthy() after begin() ==="
              << std::endl;
    std::string src =
        readFile(projectRoot() + "/src/al/pingpong/PingPongBase.h");
    assert(!src.empty());
    // The bringUpLink helper has the gate: if begin()
    // returns and isHealthy() is false, the loop blinks
    // forever (300 ms on/off) so an operator at the
    // bench sees the failure without watching the serial
    // log. This pin asserts the gate is still there —
    // the heap-refusal abort path depends on it to
    // actually halt the boot sequence visibly.
    // Find the function definition (after the comments).
    auto p = src.find("inline void bringUpLink(");
    assert(p != std::string::npos);
    std::string slice = src.substr(p, 3000);
    // The gate is `if (!comm.isHealthy())` inside
    // bringUpLink. The fix only works if that gate
    // halts the boot on a begin() that returned with
    // healthy=false (i.e. the EspHal abort path).
    assert(slice.find("!comm.isHealthy()") != std::string::npos &&
           "bringUpLink must check !comm.isHealthy()");
    assert(slice.find("HAL not healthy") != std::string::npos &&
           "bringUpLink must log a halt message");
    assert(slice.find("while (true)") != std::string::npos &&
           "bringUpLink must halt in an infinite loop, not fall through");
    // Visible halt: blink the LED every 300 ms in an infinite loop
    // so the operator sees the failure without watching the debug
    // Serial — a plain delay(1000) loop is silent.
    assert(slice.find("blinkWait(1, 300, 300, 0)") != std::string::npos &&
           "halt loop must blink the LED at 300 ms cadence");
    assert(slice.find("delay(1000)") == std::string::npos &&
           "halt loop must not be a silent delay(1000) loop");
    std::cout << "  PASS (isHealthy() check + log + 300 ms blink loop)"
              << std::endl;
}

} // namespace

int main() {
    std::cout << "=== Running EspHal Stream-Buffer Abort Tests ==="
              << std::endl;
    test_esphal_begin_aborts_on_stream_buf_failure();
    test_pingpongbase_halts_on_unhealthy();
    std::cout << "\n=== EspHal Stream-Buffer Abort Tests PASS ===" << std::endl;
    return 0;
}

#endif // !ARDUINO