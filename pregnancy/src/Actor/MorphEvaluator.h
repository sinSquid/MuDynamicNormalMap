#pragma once

namespace PNS {

    class MorphEvaluator {
    public:
        // Get the effective morph value for an actor, combining all configured morphs
        // using MorphCombineMode, with per-morph key aggregation via MorphMode.
        static float GetMorphValue(RE::Actor* actor);

        // Compute which texture partition (1-based) corresponds to the morph value.
        // Returns 0 when morph is at/below minimum (no texture override needed).
        // partitionCount is the number of texture stages available (e.g. 5 for p1-p5).
        static int ComputePartition(float morphValue, int partitionCount);

    private:
        // Compute the value for a single morph name by aggregating across its keys.
        static float GetSingleMorphValue(RE::Actor* actor, const std::string& morphName);
    };

}  // namespace PNS
