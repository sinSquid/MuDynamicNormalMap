#include "pch.h"
#include "Actor/MorphEvaluator.h"
#include "Core/Config.h"
#include "Compat/ProviderMorphs.h"
#include "Core/SKEEInterface.h"

namespace PNS {

    float MorphEvaluator::GetSingleMorphValue(RE::Actor* actor, const std::string& morphName) {
        auto& skee = SKEEInterface::GetSingleton();
        auto& config = Config::GetSingleton();
        const auto& ignoreKeys = config.GetIgnoreMorphKeys();

        auto keys = skee.GetMorphKeys(actor, morphName.c_str());
        logger::debug("  GetSingleMorphValue: {:08X} morphName='{}' keys={}", actor->GetFormID(), morphName, keys.size());

        if (keys.empty()) return 0.0f;

        float val = 0.0f;
        int mode = config.GetMorphMode();

        auto isIgnored = [&ignoreKeys, &config](const std::string& key) {
            return !ProviderMorphs::PregnancySource(key, config.IncludePresetMorphsInPregnancy(),
                config.GetPregnancyMorphKeys(), ignoreKeys);
        };

        if (mode == 0) {  // Additive
            for (const auto& key : keys) {
                float v = skee.GetMorph(actor, morphName.c_str(), key.c_str());
                bool ignored = isIgnored(key);
                logger::debug("    key='{}' val={:.4f} ignored={}", key, v, ignored);
                if (!ignored) {
                    val += v;
                }
            }
        } else if (mode == 1) {  // Averaged
            int cnt = 0;
            for (const auto& key : keys) {
                float v = skee.GetMorph(actor, morphName.c_str(), key.c_str());
                bool ignored = isIgnored(key);
                logger::debug("    key='{}' val={:.4f} ignored={}", key, v, ignored);
                if (!ignored) {
                    val += v;
                    cnt++;
                }
            }
            if (cnt > 0) val /= static_cast<float>(cnt);
        } else if (mode == 2) {  // Maximum
            for (const auto& key : keys) {
                float v = skee.GetMorph(actor, morphName.c_str(), key.c_str());
                logger::debug("    key='{}' val={:.4f}", key, v);
                if (!isIgnored(key) && std::isfinite(v)) val = std::max(val, v);
            }
        }

        logger::debug("  GetSingleMorphValue: result={:.4f} (mode={})", val, mode);
        return val;
    }

    float MorphEvaluator::GetMorphValue(RE::Actor* actor) {
        if (!actor) return 0.0f;

        auto& skee = SKEEInterface::GetSingleton();
        if (!skee.GetBodyMorphInterface()) {
            logger::debug("GetMorphValue: RaceMenu body morph interface unavailable");
            return 0.0f;
        }

        auto& config = Config::GetSingleton();
        const auto& morphNames = config.GetMorphNames();

        if (morphNames.empty()) return 0.0f;

        // Single morph — fast path, no combine logic needed
        if (morphNames.size() == 1) {
            float val = GetSingleMorphValue(actor, morphNames[0]);
            logger::debug("GetMorphValue: {:08X} result={:.4f}", actor->GetFormID(), val);
            return val;
        }

        // Multiple morphs — compute each, then combine
        int combineMode = config.GetMorphCombineMode();
        float result = 0.0f;
        int count = 0;

        logger::debug("GetMorphValue: {:08X} morphs={} combineMode={}", actor->GetFormID(), morphNames.size(), combineMode);

        for (const auto& name : morphNames) {
            float v = GetSingleMorphValue(actor, name);

            if (combineMode == 0) {  // Additive
                result += v;
            } else if (combineMode == 1) {  // Average
                result += v;
            } else if (combineMode == 2) {  // Maximum
                result = (count == 0) ? v : std::max(result, v);
            }
            count++;
        }

        if (combineMode == 1 && count > 0) {
            result /= static_cast<float>(count);
        }

        logger::debug("GetMorphValue: {:08X} combined={:.4f} (combineMode={})", actor->GetFormID(), result, combineMode);
        return result;
    }

    int MorphEvaluator::ComputePartition(float morphValue, int partitionCount) {
        auto& config = Config::GetSingleton();
        if (partitionCount <= 0) return 0;

        // Normalize morph value to 0..1 within the configured range
        const float normalized = Rendering::NormalizeMorph(morphValue, config.GetMorphMin(), config.GetMorphMax());

        // Treat values at or very near zero as "no override" — SKEE can report
        // tiny epsilon values even when the RaceMenu slider shows exactly 0.
        constexpr float kEpsilon = 1e-4f;
        if (normalized <= kEpsilon) return 0;

        // Map to 1..partitionCount (partition 0 = no override)
        int p = static_cast<int>(std::ceil(double(normalized) * partitionCount));
        return std::clamp(p, 1, partitionCount);
    }

}  // namespace PNS


