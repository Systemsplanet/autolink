// Regression: arqCacheHasRoomTrampoline
// must check pool slots not just pendingCount.
#ifndef ARDUINO

#    include <iostream>
#    include <cassert>
#    include "AutoLink.h"

using namespace autolink;

void test_trampoline_true_when_empty()
{
    std::cout << "\n=== Test: trampoline returns true when ARQ "
                 "cache is empty ==="
              << std::endl;
    AutoLink al(0, 16, 17, true);
    bool ok = AutoLink::test_arqHasRoomTrampoline(&al);
    assert(ok);
    assert(al.test_arqCache_hasRoom());
    std::cout << "PASS (pendingCount_=0, pool empty -> hasRoom)" << std::endl;
}

void test_trampoline_false_when_pool_exhausted()
{
    std::cout << "\n=== Test: trampoline returns false when pool "
                 "is exhausted (even with pendingCount_ < SLOTS) "
                 "==="
              << std::endl;
    AutoLink al(0, 16, 17, true);
    al.test_arqFillPoolForTest();
    assert(!al.test_arqCache_hasRoom());
    bool ok = AutoLink::test_arqHasRoomTrampoline(&al);
    assert(!ok);
    std::cout << "PASS (pendingCount_=0 but all " << al.test_arqPoolSize()
              << " pool slots used -> not hasRoom)" << std::endl;
}

void test_trampoline_false_when_pending_full()
{
    std::cout << "\n=== Test: trampoline returns false when "
                 "pendingCount_ == ARQ_CACHE_SLOTS ==="
              << std::endl;
    AutoLink al(0, 16, 17, true);
    al.test_arqFillPendingForTest();
    bool ok = AutoLink::test_arqHasRoomTrampoline(&al);
    assert(!ok);
    std::cout << "PASS (pendingCount_=ARQ_CACHE_SLOTS -> not "
                 "hasRoom)"
              << std::endl;
}

void test_trampoline_null_ctx()
{
    std::cout << "\n=== Test: trampoline returns false on null "
                 "ctx (defensive) ==="
              << std::endl;
    bool ok = AutoLink::test_arqHasRoomTrampoline(nullptr);
    assert(!ok);
    std::cout << "PASS (null ctx -> not hasRoom)" << std::endl;
}

int main()
{
    std::cout << "=== Running ARQ HasRoom-Trampoline Tests ===" << std::endl;
    test_trampoline_true_when_empty();
    test_trampoline_false_when_pool_exhausted();
    test_trampoline_false_when_pending_full();
    test_trampoline_null_ctx();
    std::cout << "\n=== ARQ HasRoom-Trampoline Tests Completed "
                 "Successfully ==="
              << std::endl;
    return 0;
}

#endif