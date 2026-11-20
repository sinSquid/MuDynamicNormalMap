#pragma once
#include <cstdint>
#include <functional>
#include <SKSE/SKSE.h>

namespace PNS {
    bool ModuleEnabled();
    bool SessionActive();
    std::uint64_t SessionGeneration();
    // Event callbacks are queued by ID/handle, never by a borrowed actor pointer.
    // A callback from the previous save is discarded before resolving its actor.
    void QueueSessionTask(std::function<void()> task);
    void HandleMessage(SKSE::MessagingInterface::Message* message);
    void TickFrame();
    void Shutdown();
}
