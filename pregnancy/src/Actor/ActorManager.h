#pragma once
#include "Rendering/DynamicNormalMap.h"

namespace PNS {

    struct ActorState {
        RE::FormID formID = 0;
        std::string currentTexture;    // currently applied texture path
        int currentPartition = -1;     // -1 = never evaluated
        std::string raceEditorID;      // cached race editor ID
        std::string nodeName;          // cached NiOverride node name
        bool isFemale = false;
        float currentMorph = 0.0f;
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

        std::unordered_map<RE::FormID, ActorState> trackedActors_;
        std::mutex mutex_;
    };

}  // namespace PNS
