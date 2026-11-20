#pragma once
#include "Rendering/DynamicNormalMap.h"
#include <mutex>
#include <unordered_map>

namespace PNS {

    struct ActorState {
        RE::FormID formID = 0;
        std::string currentTexture;    // currently applied texture path
        int currentPartition = -1;     // -1 = never evaluated
        std::string raceEditorID;      // cached race editor ID
        std::string nodeName;          // cached NiOverride node name
        bool isFemale = false;
        float currentMorph = 0.0f;
        bool restorePending = false;
        bool restoreQueued = false;
        std::uint64_t restoreRequest = 0;

        void SuppressLayer(bool installedLayer) {
            restorePending = restorePending || installedLayer || currentMorph != 0.0f ||
                currentPartition > 0 || !currentTexture.empty();
            // Logical invalidation precedes the independent physical restore.
            currentMorph = 0.0f;
            currentPartition = 0;
            currentTexture.clear();
        }

        template <class Enqueue>
        void ScheduleRestore(std::uint64_t request, Enqueue&& enqueue) {
            if (!restorePending || restoreQueued) return;
            restoreRequest = request;
            restoreQueued = true;
            try {
                if (!enqueue()) restoreQueued = false;
            } catch (...) {
                restoreQueued = false;
                throw;
            }
        }
        bool TakeRestore(std::uint64_t request) {
            if (!restorePending || !restoreQueued || restoreRequest != request) return false;
            restoreQueued = false;
            return true;
        }
        bool FinishRestore(std::uint64_t request, bool restored) {
            if (restoreRequest != request) return false;
            restorePending = !restored;
            return true;
        }
    };

    class ActorManager {
    public:
        static ActorManager& GetSingleton() {
            static ActorManager instance;
            return instance;
        }

        // Process a single actor (evaluate morph, apply/remove texture)
        void ProcessActor(RE::Actor* actor, bool forceUpdate = false);
        void UpdatePregnancy(RE::Actor* actor, bool forceUpdate = false);

        // Process all currently loaded actors with ActorTypeNPC
        void ProcessAllLoaded();

        // Clear all tracked actor state (on new game)
        void Clear();

        // Remove tracking for a specific actor
        void RemoveActor(RE::FormID formID);

        // Called periodically by the polling task
        void PollUpdate();

        // Get or create state for an actor
        ActorState& GetOrCreateState(RE::Actor* actor);

        // Check if an actor should be processed by PNS
        bool ShouldProcess(RE::Actor* actor) const;
        std::pair<float, std::string> GetPregnancyLayer(RE::FormID actor);

    private:
        ActorManager() = default;
        // Called with mutex_ held; the queue never invokes the callback inline.
        void QueueRestore(RE::FormID id, ActorState& state);
        void RunRestore(RE::FormID id, std::uint64_t session, std::uint64_t request);

        std::unordered_map<RE::FormID, ActorState> trackedActors_;
        std::mutex mutex_;
        std::uint64_t nextRestoreRequest_ = 0;
    };

}  // namespace PNS
