#pragma once

namespace PNS {

    // Optional Skill Based Muscle integration
    class SBMCompat {
    public:
        static SBMCompat& GetSingleton() {
            static SBMCompat instance;
            return instance;
        }

        // Check if Skill Based Muscle is installed (call during kDataLoaded)
        void Init();

        bool IsInstalled() const { return installed_; }

        // Get SBM muscle level for an actor (1-based, 0 = not applicable)
        int GetMuscleLevel(RE::Actor* actor) const;

    private:
        SBMCompat() = default;

        bool installed_ = false;
    };

}  // namespace PNS
