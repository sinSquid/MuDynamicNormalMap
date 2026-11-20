#pragma once
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

namespace PNS::OSkin {
    using MaterialAssignment = std::tuple<std::uintptr_t, std::uintptr_t, std::string>;

    // Acknowledge only our own replacement of an already observed source. An
    // external material/path change must remain visible to the next Observe.
    inline void AcknowledgeClone(std::vector<MaterialAssignment>& observed,
        const MaterialAssignment& source, const MaterialAssignment& replacement) {
        if (std::get<0>(source) != std::get<0>(replacement) ||
            std::get<2>(source) != std::get<2>(replacement)) return;
        for (auto& assignment : observed) {
            if (assignment == source) {
                assignment = replacement;
                return;
            }
        }
    }
}
