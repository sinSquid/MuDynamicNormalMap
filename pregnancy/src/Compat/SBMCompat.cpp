#include "pch.h"
#include "Compat/SBMCompat.h"

namespace PNS {

    void SBMCompat::Init() {
        auto* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) return;

        installed_ = dataHandler->LookupModByName("Skill Based Muscle.esp") != nullptr;

        if (installed_) {
            logger::info("SBMCompat: Skill Based Muscle detected");
        } else {
            logger::info("SBMCompat: Skill Based Muscle not found");
        }
    }

    int SBMCompat::GetMuscleLevel([[maybe_unused]] RE::Actor* actor) const {
        if (!installed_) return 0;

        // TODO: Implement SBM muscle level calculation
        // This requires reading SBM's MCM config and computing muscle from actor skills.
        // For now, return 0 (no SBM suffix) — this is a post-launch enhancement.
        return 0;
    }

}  // namespace PNS

