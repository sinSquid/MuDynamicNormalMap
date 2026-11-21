#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <ctime>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace RE { class NiNode; class BSFaceGenNiNode; class Actor; class BGSHeadPart; class NiAVObject; }
#include "../include/Hook.h"
#include "../include/ThreadPool.h"

struct ProbeEvent {};
struct Probe final : Mus::IEventListener<ProbeEvent> {
    std::atomic<bool> subscribed{true};
    std::atomic<unsigned> staleCalls{0};
    void onEvent(const ProbeEvent&) override {
        if (!subscribed.load()) ++staleCalls;
    }
};

static DWORD_PTR CurrentAffinity() {
    GROUP_AFFINITY group{};
    assert(GetThreadGroupAffinity(GetCurrentThread(), &group));
    return group.Mask;
}

int main() {
    Mus::EventDispatcherImpl<ProbeEvent> events;
    Probe listener;
    events.addListener(&listener);
    std::atomic<bool> done{false};
    std::thread dispatch([&] { while (!done.load()) events.dispatch({}); });
    for (int pass = 0; pass < 50000; ++pass) {
        events.removeListener(&listener);
        listener.subscribed.store(false);
        std::this_thread::yield();
        listener.subscribed.store(true);
        events.addListener(&listener);
    }
    done.store(true);
    dispatch.join();
    assert(listener.staleCalls.load() == 0);
    events.removeListener(&listener);
    listener.subscribed.store(false);
    events.dispatch({});
    assert(listener.staleCalls.load() == 0);

    const auto original = CurrentAffinity();
    const auto first = original & (~original + 1);
    const auto rest = original & ~first;
    const auto second = rest ? rest & (~rest + 1) : first;
    tbb::task_arena arenaA(1), arenaB(1);
    Mus::TBB_CoreMasking a(arenaA, first), b(arenaB, second);
    a.on_scheduler_entry(true);
    assert(CurrentAffinity() == first);
    b.on_scheduler_entry(true);
    assert(CurrentAffinity() == second);
    b.on_scheduler_exit(true);
    assert(CurrentAffinity() == first);
    a.on_scheduler_exit(true);
    assert(CurrentAffinity() == original);
    // The same worker entering a recreated/different arena must get its new mask.
    b.on_scheduler_entry(true);
    assert(CurrentAffinity() == second);
    b.on_scheduler_exit(true);
    assert(CurrentAffinity() == original);
    a.on_scheduler_entry(false);
    a.on_scheduler_exit(false);
    assert(CurrentAffinity() == original);

    // Destroying an outer observer cannot overwrite a still-active inner arena,
    // and the inner exit cannot reinstate the now-dead outer arena's mask.
    auto outer = std::make_unique<Mus::TBB_CoreMasking>(arenaA, first);
    auto inner = std::make_unique<Mus::TBB_CoreMasking>(arenaB, second);
    outer->on_scheduler_entry(true);
    inner->on_scheduler_entry(true);
    outer.reset();
    assert(CurrentAffinity() == second);
    inner->on_scheduler_exit(true);
    assert(CurrentAffinity() == original);
    inner.reset();

    // Nested same-owner callbacks remain balanced without duplicate stack nodes.
    a.on_scheduler_entry(true);
    a.on_scheduler_entry(true);
    a.on_scheduler_exit(true);
    assert(CurrentAffinity() == first);
    a.on_scheduler_exit(true);
    assert(CurrentAffinity() == original);

    // An unrelated affinity setter retains ownership when an observer retires.
    outer = std::make_unique<Mus::TBB_CoreMasking>(arenaA, first);
    outer->on_scheduler_entry(true);
    SetThreadAffinityMask(GetCurrentThread(), second);
    outer.reset();
    assert(CurrentAffinity() == second);
    SetThreadAffinityMask(GetCurrentThread(), original);

    // Real TBB worker: stopping observation does not imply a worker exit callback.
    tbb::task_arena arenaWorker(2);
    outer = std::make_unique<Mus::TBB_CoreMasking>(arenaWorker, first);
    std::promise<void> entered, release;
    auto proceed = release.get_future().share();
    std::promise<DWORD_PTR> afterRemoval;
    arenaWorker.enqueue([&] {
        entered.set_value();
        proceed.wait();
        afterRemoval.set_value(CurrentAffinity());
    });
    entered.get_future().wait();
    outer.reset();
    release.set_value();
    assert(afterRemoval.get_future().get() == original);
}
