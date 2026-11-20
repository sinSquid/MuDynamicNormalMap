#include <cstdint>
namespace RE { using FormID = std::uint32_t; class Actor; }
#include "Actor/ActorManager.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

static void Check(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}

int main() {
    PNS::ActorState state;
    state.currentMorph = 0.7f;
    state.currentPartition = 3;
    state.currentTexture = "textures/PNS/stage3.dds";
    state.SuppressLayer(false);
    Check(state.currentMorph == 0 && state.currentTexture.empty() && state.currentPartition == 0,
        "Exclusion logically removes a stage before queuing physical restoration");
    int attempts = 0;
    state.ScheduleRestore(1, [&] { ++attempts; return false; });
    Check(state.restorePending && !state.restoreQueued && attempts == 1,
        "Rejected queue acceptance must preserve an unscheduled restore");
    state.ScheduleRestore(2, [&] { ++attempts; return true; });
    Check(state.restorePending && state.restoreQueued && attempts == 2,
        "Queue acceptance is not physical restore completion");
    state.SuppressLayer(false);
    state.ScheduleRestore(3, [&] { ++attempts; return true; });
    Check(attempts == 2 && state.restoreRequest == 2, "Repeated exclusion while queued is coalesced");
    Check(!state.TakeRestore(1) && state.restoreQueued, "An obsolete request cannot consume the active request");
    Check(state.TakeRestore(2) && !state.restoreQueued && state.restorePending,
        "Execution consumes only the queue ticket, not pending physical work");
    Check(!state.TakeRestore(2), "Duplicate callback is rejected");
    state.ScheduleRestore(4, [&] { ++attempts; return true; });
    Check(state.restoreQueued && state.restorePending && attempts == 3, "Physical failure can be retried");
    // Eligibility returning must not consume this request. Mu's normal queue
    // may return true for a disabled category without installing anything.
    state.ScheduleRestore(5, [&] { ++attempts; return true; });
    Check(attempts == 3 && state.restoreRequest == 4 && state.restorePending,
        "Regaining eligibility preserves the existing physical restore ticket");
    Check(!state.TakeRestore(2) && state.TakeRestore(4), "A stale callback cannot consume the restored eligibility ticket");
    Check(state.FinishRestore(4, false) && state.restorePending,
        "Physical restore failure retains pending even after eligibility returns");
    state.ScheduleRestore(5, [&] { ++attempts; return true; });
    Check(!state.FinishRestore(4, true) && state.restorePending,
        "An obsolete completion cannot clear a newer restore");
    Check(state.TakeRestore(5) && state.FinishRestore(5, true), "Only current physical completion clears pending");
    state.ScheduleRestore(6, [&] { ++attempts; return true; });
    Check(attempts == 4, "Completed restoration does not requeue");

    PNS::ActorState untouched;
    untouched.SuppressLayer(false);
    Check(!untouched.restorePending, "Actors without a logical or installed layer need no restore");
    untouched.SuppressLayer(true);
    Check(untouched.restorePending && untouched.currentMorph == 0,
        "A physical layer restores even when the logical amount is already zero");
    try {
        untouched.ScheduleRestore(7, []() -> bool { throw std::runtime_error("queue rejected"); });
        Check(false, "Queue exception must propagate");
    } catch (const std::runtime_error&) {}
    Check(untouched.restorePending && !untouched.restoreQueued, "Queue exception cannot latch the actor busy");
    untouched.ScheduleRestore(8, [] { return true; });
    Check(untouched.TakeRestore(8) && untouched.FinishRestore(8, true), "Ordinary zero physically completes");
    untouched.SuppressLayer(false);
    Check(!untouched.restorePending, "Zero re-evaluation after physical completion cannot create a restore loop");
    PNS::ActorState missingStage;
    missingStage.currentMorph = 1;
    missingStage.currentPartition = 2;
    missingStage.currentTexture = "old-race-stage.dds";
    missingStage.SuppressLayer(true);
    Check(missingStage.restorePending && missingStage.currentTexture.empty(),
        "An unavailable requested stage invalidates the old stage rather than retaining it");
    PNS::ActorState dynamic;
    dynamic.currentMorph = 0.4f;
    dynamic.currentPartition = 0;
    dynamic.SuppressLayer(false);
    Check(dynamic.restorePending && dynamic.currentMorph == 0, "Dynamic layers also restore without a stage path");
    PNS::ActorState replacement;
    Check(!replacement.TakeRestore(5), "A cleared/unloaded actor state rejects its previous callback");
    std::cout << "Actor layer invalidation, queue ownership, retry and cancellation tests passed\n";
}
