#pragma once
#include <atomic>
#include "Rendering/DynamicNormalMap.h"

namespace PNS {

    struct ForceUpdateKeyConfig {
        std::vector<std::uint32_t> keys = {42, 56, 21};  // DirectInput scan codes: LShift(42) + LAlt(56) + Y(21)
    };

    struct ActorFilterConfig {
        bool excludeChildren = true;
        std::vector<std::string> excludeKeywords;
        bool requireFemale = true;
    };

    class Config {
    public:
        static Config& GetSingleton() {
            static Config instance;
            return instance;
        }

        void Load();
        void Reload();
        bool IsModuleEnabled() const { return moduleEnabled_.load(); }

        // Morph settings
        const std::vector<std::string>& GetMorphNames() const { return morphNames_; }
        bool IncludePresetMorphsInPregnancy() const { return includePresetMorphsInPregnancy_; }
        const std::vector<std::string>& GetPregnancyMorphKeys() const { return pregnancyMorphKeys_; }
        int GetMorphMode() const { return morphMode_; }
        int GetMorphCombineMode() const { return morphCombineMode_; }
        float GetMorphMin() const { return morphMin_; }
        float GetMorphMax() const { return morphMax_; }
        int GetPartitions() const { return partitions_; }
        const DynamicTextureConfig& GetDynamicTextures() const { return dynamicTextures_; }

        // Ignored morph keys
        const std::vector<std::string>& GetIgnoreMorphKeys() const { return ignoreMorphKeys_; }

        // Node names to search
        const std::vector<std::string>& GetNodeNames() const { return nodeNames_; }

        // Player settings
        bool IsPlayerEnabled() const { return enablePlayer_; }

        // Hotkey
        const ForceUpdateKeyConfig& GetForceUpdateKey() const { return forceUpdateKey_; }

        // Polling
        float GetUpdateInterval() const { return updateInterval_; }

        // Debug
        bool IsDebugMode() const { return debugMode_; }
        float GetDebugMorphChangeMult() const { return debugMorphChangeMult_; }

        // Actor filter
        const ActorFilterConfig& GetActorFilter() const { return actorFilter_; }

        // Texture base directory (relative to Data/)
        const std::string& GetBaseDirectory() const { return baseDirectory_; }

    private:
        Config() = default;

        std::atomic<bool> moduleEnabled_ = false;

        DynamicTextureConfig dynamicTextures_;

        std::vector<std::string> morphNames_ = {"PregnancyBelly"};
        bool includePresetMorphsInPregnancy_ = false;
        std::vector<std::string> pregnancyMorphKeys_;
        int morphMode_ = 0;
        int morphCombineMode_ = 0;  // 0=additive, 1=average, 2=maximum
        float morphMin_ = 0.0f;
        float morphMax_ = 1.0f;
        int partitions_ = 0;  // 0 = auto-detect from textures on disk
        std::vector<std::string> ignoreMorphKeys_;
        std::vector<std::string> nodeNames_ = {"3BA", "3BA_Naked", "3BBB", "CBBE", "BHUNP", "UUNP", "UNP", "BaseShape", "FemaleBody", "UBE", "UBE Body"};
        bool enablePlayer_ = true;
        ForceUpdateKeyConfig forceUpdateKey_;
        float updateInterval_ = 0.5f;
        bool debugMode_ = false;
        float debugMorphChangeMult_ = 10.0f;
        ActorFilterConfig actorFilter_;
        std::string baseDirectory_ = "textures/PNS";

        mutable std::shared_mutex mutex_;
    };

}  // namespace PNS
