#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace PNS::ProviderMorphs {
    // Body (slot 32) plus separate genital geometry (slot 52).
    constexpr std::uint32_t bodySlots = (1u << 2) | (1u << 22);
    constexpr std::uint32_t allSlots = UINT32_MAX;

    inline bool IsPresetKey(std::string_view key) { return key == "OBody" || key == "OClothe"; }

    inline bool PregnancySource(const std::string& key, bool includePreset,
        const std::vector<std::string>& allowed, const std::vector<std::string>& ignored) {
        if (std::ranges::find(ignored, key) != ignored.end()) return false;
        if (!allowed.empty()) return std::ranges::find(allowed, key) != allowed.end();
        if (IsPresetKey(key)) return includePreset;
        return key != "ArousedBodyMorphs.esp" && key != "ABBA.esp" && key != "DinoRMF_Preview";
    }

    struct Providers {
        bool obody = false;
        bool abba = false;
        bool abm = false;
        bool slif = false;

        bool Active() const { return obody || abba || abm; }
    };

    using Entry = std::tuple<std::string, std::string, float>;
    struct Snapshot {
        std::vector<Entry> preset;
        std::vector<Entry> adjustment;

        void Add(const char* slider, const char* key, float value, const Providers& providers) {
            if (!slider || !key || !std::isfinite(value) || value == 0) return;
            if (providers.obody && IsPresetKey(key)) preset.emplace_back(slider, key, value);
            else if ((providers.abba && std::string_view(key) == "ABBA.esp") ||
                     (providers.abm && std::string_view(key) == "ArousedBodyMorphs.esp") ||
                     (providers.abba && providers.slif))
                adjustment.emplace_back(slider, key, value);
        }

        // RaceMenu's traversal order can differ even if the stored values are identical.
        std::uint32_t ChangedSlots(const Snapshot& previous) {
            std::sort(preset.begin(), preset.end());
            std::sort(adjustment.begin(), adjustment.end());
            if (preset != previous.preset) return allSlots;
            return adjustment != previous.adjustment ? bodySlots : 0u;
        }
        bool Empty() const { return preset.empty() && adjustment.empty(); }
    };
}

