#pragma once

namespace PNS {

    struct ActorState;

    class NormalMapApplicator {
    public:
        // Evaluate the actor's morph and apply/remove normal map override as needed.
        // Returns true if the texture was changed.
        static bool Apply(RE::Actor* actor, ActorState& state, bool forceUpdate = false);

        // Queue restoration through Mu; preserve state when enqueue is rejected.
        static bool Reset(RE::Actor* actor, ActorState& state);

        // Find the correct NiOverride node name for an actor's body mesh.
        static std::string FindNodeName(RE::Actor* actor);

        // Debug: cycle through partitions on the player. Returns the new partition.
        static int DebugCyclePartition();

        // Debug: get the current debug partition override (-1 = disabled)
        static int GetDebugPartitionOverride() { return debugPartition_; }

    private:
        static inline int debugPartition_ = -1;  // -1 = use morph value, 0+ = forced partition
    };

}  // namespace PNS
