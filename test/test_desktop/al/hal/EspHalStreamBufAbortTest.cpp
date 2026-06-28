// Source-level pin for the EspHal::begin() heap-refusal
// abort path. xStreamBufferCreate failure used to log
// an error and fall through — the link task came up
// with a null stream_buf, every app-buf push was
// silently dropped, and bringUpLink's isHealthy() gate
// saw healthy=true anyway because that flag is set at
// the bottom of begin() after every other RTOS primitive
// passed. The link looked up but every received byte
// was thrown away — a silent wire.
//
// This release fixes it: the xStreamBufferCreate
// failure branch now cleans up the mutex + task_exit_sem,
// sets running=false, and returns without setting
// healthy=true. The link stays down; bringUpLink's
// !isHealthy() halt loop fires.
//
// The fix is `#ifdef ARDUINO`-only and lives in
// EspHal.h. EspHal.cpp doesn't exist; the class is
// entirely inline. The pin is source-grep only — the
// shape is what matters, and the shape is plain text.
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
    std::string src = readFile(projectRoot() + "/src/al/hal/EspHal.h");
    assert(!src.empty());

    // Locate the xStreamBufferCreate call site.
    auto p = src.find("xStreamBufferCreate(stream_buf_size_, 1)");
    assert(p != std::string::npos);

    // The fix shape: when the create returns null, the
    // body must (a) log, (b) clean up mutex +
    // task_exit_sem, (c) set running=false, (d) return.
    // Pre-fix shape logged and fell through. Read the
    // ~2500 chars after the create call and assert all
    // four signals are present. The slice is wide
    // because the cleanup is verbose (mutex delete +
    // task_exit_sem delete + log + return).
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

    // The pre-fix log-only message ("check heap") must
    // be gone. The fix replaces it with "aborting
    // begin(), link stays down".
    assert(slice.find("check heap") == std::string::npos &&
           "pre-fix 'check heap' message must be replaced");
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
    // The bringUpLink helper already has the gate
    // (post-fix from an earlier release): if begin()
    // returns and isHealthy() is false, the loop hangs
    // forever with an error log. This pin asserts the
    // gate is still there — the new heap-refusal abort
    // path depends on it to actually halt the boot
    // sequence.
    // Find the function definition (after the comments).
    auto p = src.find("inline void bringUpLink(");
    assert(p != std::string::npos);
    std::string slice = src.substr(p, 2500);
    // The pre-this-release gate is `if (!comm.isHealthy())`
    // inside bringUpLink. The fix only works if that gate
    // still halts the boot on a begin() that returned with
    // healthy=false (i.e. the EspHal abort path).
    assert(slice.find("!comm.isHealthy()") != std::string::npos &&
           "bringUpLink must check !comm.isHealthy()");
    assert(slice.find("HAL not healthy") != std::string::npos &&
           "bringUpLink must log a halt message");
    assert(slice.find("while (true)") != std::string::npos &&
           "bringUpLink must halt in a delay loop, not fall through");
    assert(slice.find("delay(1000)") != std::string::npos &&
           "the halt loop must call delay(1000)");
    std::cout << "  PASS (isHealthy() check + log + halt loop)" << std::endl;
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