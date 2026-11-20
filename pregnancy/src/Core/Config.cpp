#include "pch.h"
#include "Core/Config.h"

namespace PNS {

    void Config::Load() {
        std::unique_lock lock(mutex_);
        moduleEnabled_.store(false);
        dynamicTextures_ = DynamicTextureConfig{};
        dynamicTextures_.profiles = {
            {"CBBE/3BA", {"3BA", "3BA_Naked", "3BA_Clothing", "3BBB", "CBBE", "BaseShape"}, {}},
            {"BHUNP/UNP", {"BHUNP", "BHUNP 3BBB", "UUNP", "UNP", "UNPB"}, {}},
            {"Vanilla", {"FemaleBody", "FemaleTorso", "femalebody_0", "femalebody_1"}, {}},
            {"UBE", {"UBE", "UBE Body", "UBEBody", "UBE 3BBB", "UBE_3BA"}, {}}
        };

        auto path = fs::path("Data") / "SKSE" / "Plugins" / ConfigDirectory / "Config.json";
        logger::info("Config: Looking for config at: {}", path.string());

        if (!fs::exists(path)) {
            logger::error("Config: Config.json not found at {}", path.string());
            return;
        }

        std::ifstream file(path);
        if (!file.is_open()) {
            logger::error("Config: Failed to open {}", path.string());
            return;
        }

        try {
            json j = json::parse(file);

            if (j.contains("MorphNames") && j["MorphNames"].is_array()) {
                morphNames_.clear();
                for (const auto& name : j["MorphNames"]) {
                    morphNames_.push_back(name.get<std::string>());
                }
            } else if (j.contains("MorphName")) {
                morphNames_ = { j["MorphName"].get<std::string>() };
            }
            includePresetMorphsInPregnancy_ = j.value("IncludePresetMorphsInPregnancy", false);
            pregnancyMorphKeys_ = j.value("PregnancyMorphKeys", std::vector<std::string>{});
            if (j.contains("MorphMode")) morphMode_ = j["MorphMode"].get<int>();
            if (j.contains("MorphCombineMode")) morphCombineMode_ = j["MorphCombineMode"].get<int>();
            if (j.contains("MorphValueMin")) morphMin_ = j["MorphValueMin"].get<float>();
            if (j.contains("MorphValueMax")) morphMax_ = j["MorphValueMax"].get<float>();
            if (j.contains("NumberOfPartitions")) partitions_ = j["NumberOfPartitions"].get<int>();
            if (j.contains("EnablePlayer")) enablePlayer_ = j["EnablePlayer"].get<int>() != 0;
            if (j.contains("UpdateInterval")) updateInterval_ = j["UpdateInterval"].get<float>();
            if (j.contains("EnableDebugMode")) debugMode_ = j["EnableDebugMode"].get<int>() != 0;
            if (j.contains("DebugMorphChangeMult")) debugMorphChangeMult_ = j["DebugMorphChangeMult"].get<float>();

            if (j.contains("DynamicTextures")) {
                const auto& d = j.at("DynamicTextures");
                dynamicTextures_.enabled = d.value("Enabled", true);
                dynamicTextures_.bodyProfile = d.value("BodyProfile", std::string("Auto"));
                dynamicTextures_.usePregnancyTarget = d.value("UsePregnancyTarget", true);
                dynamicTextures_.useActorSkin = d.value("UseActorSkin",false);
                dynamicTextures_.baseNormal = d.value("BaseNormal",dynamicTextures_.baseNormal);
                dynamicTextures_.smoothing = std::clamp(d.value("Smoothing",0.85f),0.0f,1.0f);
                dynamicTextures_.blurRadius = std::clamp(d.value("BlurRadius",0.008f),0.0001f,0.05f);
                const int limit = d.value("MaxResolution", 0);
                dynamicTextures_.maxResolution = limit > 0 ? static_cast<unsigned>(std::clamp(limit,256,8192)) : 0;
                if (d.contains("Profiles")) for (auto& profile : dynamicTextures_.profiles) {
                    if (!d["Profiles"].contains(profile.name)) continue;
                    const auto& p = d["Profiles"][profile.name];
                    profile.nodes = p.value("NodeNames",profile.nodes);
                    profile.pregnancyNormal = p.value("PregnancyNormal",std::string{});
                    profile.baseNormal = p.value("BaseNormal",std::string{});
                    profile.belly.centerHeight = std::clamp(p.value("CenterHeight",0.69f),0.0f,1.0f);
                    profile.belly.radiusHeight = std::clamp(p.value("RadiusHeight",0.12f),0.001f,1.0f);
                    profile.belly.radiusWidth = std::clamp(p.value("RadiusWidth",0.105f),0.001f,1.0f);
                    profile.belly.frontSign = p.value("FrontSign",1.0f) >= 0 ? 1.0f : -1.0f;
                    profile.belly.breastCenterHeight = std::clamp(p.value("BreastCenterHeight",0.83f),0.0f,1.0f);
                    profile.belly.breastCenterOffset = std::clamp(p.value("BreastCenterOffset",0.07f),0.0f,1.0f);
                    profile.belly.breastRadiusWidth = std::clamp(p.value("BreastRadiusWidth",0.063f),0.001f,1.0f);
                    profile.belly.breastRadiusHeight = std::clamp(p.value("BreastRadiusHeight",0.065f),0.001f,1.0f);
                }
            }
            if (dynamicTextures_.bodyProfile != "Auto" && std::none_of(dynamicTextures_.profiles.begin(),dynamicTextures_.profiles.end(),[&](const auto& p){return p.name == dynamicTextures_.bodyProfile;})) {
                logger::warn("Unknown body profile '{}'; using Auto",dynamicTextures_.bodyProfile);
                dynamicTextures_.bodyProfile = "Auto";
            }
            if (!std::isfinite(morphMin_) || !std::isfinite(morphMax_) || morphMax_ <= morphMin_) {
                logger::warn("Invalid morph range; using 0..1"); morphMin_ = 0.0f; morphMax_ = 1.0f;
            }
            if (!std::isfinite(updateInterval_) || updateInterval_ < 0) updateInterval_ = 0.5f;

            if (j.contains("IgnoreMorphKeys") && j["IgnoreMorphKeys"].is_array()) {
                ignoreMorphKeys_.clear();
                for (const auto& key : j["IgnoreMorphKeys"]) {
                    ignoreMorphKeys_.push_back(key.get<std::string>());
                }
            }

            if (j.contains("NodeNames") && j["NodeNames"].is_array()) {
                nodeNames_.clear();
                for (const auto& name : j["NodeNames"]) {
                    nodeNames_.push_back(name.get<std::string>());
                }
            }

            if (j.contains("ForceUpdateKey") && j["ForceUpdateKey"].is_array()) {
                forceUpdateKey_.keys.clear();
                for (const auto& k : j["ForceUpdateKey"]) {
                    forceUpdateKey_.keys.push_back(k.get<std::uint32_t>());
                }
            }

            if (j.contains("ActorFilter") && j["ActorFilter"].is_object()) {
                auto& af = j["ActorFilter"];
                if (af.contains("excludeChildren")) actorFilter_.excludeChildren = af["excludeChildren"].get<bool>();
                if (af.contains("requireFemale")) actorFilter_.requireFemale = af["requireFemale"].get<bool>();
                if (af.contains("excludeKeywords") && af["excludeKeywords"].is_array()) {
                    actorFilter_.excludeKeywords.clear();
                    for (const auto& kw : af["excludeKeywords"]) {
                        actorFilter_.excludeKeywords.push_back(kw.get<std::string>());
                    }
                }
            }

            // Publish the opt-in only after the entire configuration was parsed.
            moduleEnabled_.store(j.value("ModuleEnabled", false));
            logger::info("Pregnancy config loaded; ModuleEnabled={}", IsModuleEnabled());
            std::string morphNamesStr;
            for (size_t i = 0; i < morphNames_.size(); ++i) {
                if (i > 0) morphNamesStr += ", ";
                morphNamesStr += morphNames_[i];
            }
            logger::info("  MorphNames=[{}], Mode={}, CombineMode={}, Min={}, Max={}, Partitions={}",
                         morphNamesStr, morphMode_, morphCombineMode_, morphMin_, morphMax_, partitions_);
            logger::info("  EnablePlayer={}, UpdateInterval={}, Debug={}",
                         enablePlayer_, updateInterval_, debugMode_);
            logger::info("  NodeNames count={}, IgnoreMorphKeys count={}",
                         nodeNames_.size(), ignoreMorphKeys_.size());

        } catch (const json::exception& e) {
            logger::error("Config: JSON parse error: {}", e.what());
        }
    }

    void Config::Reload() {
        logger::info("Config: Reloading...");
        Load();
    }

}  // namespace PNS

