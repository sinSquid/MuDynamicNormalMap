#pragma once

namespace PNS {

    struct ActorState;

    class NormalMapApplicator {
    public:
        // Evaluate the actor's morph and apply/remove normal map override as needed.
        // Returns true when a replacement request was accepted. Suppression can
        // instead mark state.restorePending for ActorManager's independent queue.
        static bool Apply(RE::Actor* actor, ActorState& state, bool forceUpdate = false, bool installedLayer = false);

        // Invalidate an old layer for independent restoration, or request an
        // ordinary baseline refresh when no owned layer remains.
        static bool Reset(RE::Actor* actor, ActorState& state, bool installedLayer = false);

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
