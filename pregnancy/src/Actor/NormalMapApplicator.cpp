#include "pch.h"
#include "Actor/NormalMapApplicator.h"
#include "Actor/ActorManager.h"
#include "Actor/MorphEvaluator.h"
#include "Core/Config.h"
#include "Core/SKEEInterface.h"
#include "Core/TextureCache.h"
#include "Rendering/MuBridge.h"

namespace PNS {

    bool NormalMapApplicator::Apply(RE::Actor* actor, ActorState& state, bool forceUpdate) {
        if (!actor || !actor->Is3DLoaded()) {
            logger::debug("Apply: actor null or 3D not loaded (actor={:p})", (void*)actor);
            return false;
        }

        auto& skee = SKEEInterface::GetSingleton();
        if (!skee.GetBodyMorphInterface()) return false;

        if (Config::GetSingleton().GetDynamicTextures().enabled) {
            const auto& config = Config::GetSingleton();
            float amount = Rendering::NormalizeMorph(MorphEvaluator::GetMorphValue(actor),config.GetMorphMin(),config.GetMorphMax());
            if (debugPartition_ >= 0 && actor == RE::PlayerCharacter::GetSingleton()) amount = std::clamp(debugPartition_ / 10.0f,0.0f,1.0f);
            // The first observation and forced equipment refresh must also queue
            // zero, so a previously installed pregnancy normal is restored by Mu.
            if (state.currentPartition >= 0 && amount == state.currentMorph && !forceUpdate) return false;
            if (!QueueMuBody(actor)) return false;
            state.currentMorph = amount;
            state.currentPartition = 0;
            state.currentTexture.clear();
            return true;
        }

        // Re-check node name if empty (e.g. after equipment change)
        if (state.nodeName.empty()) {
            state.nodeName = FindNodeName(actor);
            if (state.nodeName.empty()) {
                logger::debug("Apply: No valid body node for {:08X} (tried {} node names)",
                             actor->GetFormID(), Config::GetSingleton().GetNodeNames().size());
                return false;
            }
            logger::debug("Apply: Found node '{}' for {:08X}", state.nodeName, actor->GetFormID());
        }

        // Check if race changed
        if (auto* race = actor->GetRace()) {
            auto currentRace = std::string(race->GetFormEditorID());
            if (currentRace != state.raceEditorID) {
                logger::debug("Apply: Race changed for {:08X}: '{}' -> '{}'",
                             actor->GetFormID(), state.raceEditorID, currentRace);
                state.raceEditorID = currentRace;
                state.nodeName = FindNodeName(actor);
                forceUpdate = true;
            }
        }

        auto& cache = TextureCache::GetSingleton();
        auto raceDir = cache.ResolveRaceDir(state.raceEditorID);
        logger::debug("Apply: {:08X} race='{}' resolvedDir='{}' isFemale={} node='{}'",
                     actor->GetFormID(), state.raceEditorID, raceDir, state.isFemale, state.nodeName);

        // Determine partition count: use config override if set, otherwise auto-detect from textures
        int partitionCount = Config::GetSingleton().GetPartitions();
        auto* raceInfo = cache.GetRaceInfo(raceDir);
        if (partitionCount <= 0) {
            partitionCount = raceInfo ? raceInfo->maxPartition : 0;
            logger::debug("Apply: Auto-detected partitionCount={} from textures (raceInfo={:p})",
                         partitionCount, (void*)raceInfo);
        } else {
            logger::debug("Apply: Config override partitionCount={} (raceInfo maxPartition={})",
                         partitionCount, raceInfo ? raceInfo->maxPartition : -1);
        }

        float morphVal = MorphEvaluator::GetMorphValue(actor);
        int partition;

        // Debug partition override: skip morph evaluation when cycling
        if (debugPartition_ >= 0 && actor == RE::PlayerCharacter::GetSingleton()) {
            partition = debugPartition_;
            logger::debug("Apply: {:08X} DEBUG override partition={} (partitionCount={})",
                         actor->GetFormID(), partition, partitionCount);
        } else {
            partition = MorphEvaluator::ComputePartition(morphVal, partitionCount);
            logger::debug("Apply: {:08X} morphVal={:.4f} partitionCount={} -> partition={}  (min={} max={})",
                         actor->GetFormID(), morphVal, partitionCount, partition,
                         Config::GetSingleton().GetMorphMin(), Config::GetSingleton().GetMorphMax());
        }

        // Partition 0 means no pregnancy texture needed
        if (partition == 0) {
            if (state.currentPartition != 0 || forceUpdate) {
                logger::debug("Apply: {:08X} partition=0, resetting from p={}", actor->GetFormID(), state.currentPartition);
                return Reset(actor, state);
            }
            logger::debug("Apply: {:08X} partition=0, already at default", actor->GetFormID());
            return false;
        }

        // Compute the texture path
        float actorWeight = actor->GetActorBase() ? actor->GetActorBase()->GetWeight() : 50.0f;

        auto texPath = cache.Get(raceDir, state.isFemale, actorWeight, partition);
        logger::debug("Apply: Cache lookup: race='{}' female={} weight={:.1f} p={} -> '{}'",
                     raceDir, state.isFemale, actorWeight, partition,
                     texPath.empty() ? "(NOT FOUND)" : texPath);

        if (texPath.empty()) {
            return false;
        }

        // Skip if same texture already applied
        if (!forceUpdate && texPath == state.currentTexture) {
            logger::debug("Apply: {:08X} same texture already applied, skipping", actor->GetFormID());
            return false;
        }

        if (!QueueMuBody(actor)) return false;
        state.currentMorph = 1.0f;
        state.currentPartition = partition;
        state.currentTexture = texPath;
        return true;
    }

    bool NormalMapApplicator::Reset(RE::Actor* actor, ActorState& state) {
        if (!QueueMuBody(actor)) return false;
        state.currentMorph = 0;
        state.currentPartition = 0;
        state.currentTexture.clear();

        logger::debug("NormalMapApplicator: Reset normal map for {:08X}", actor->GetFormID());
        return true;
    }

    std::string NormalMapApplicator::FindNodeName(RE::Actor* actor) {
        if (!actor) return "";

        auto* root = actor->Get3D();
        if (!root) {
            logger::debug("FindNodeName: {:08X} has no 3D root", actor->GetFormID());
            return "";
        }

        logger::debug("FindNodeName: {:08X} root node='{}' type='{}'",
                     actor->GetFormID(), root->name.c_str(),
                     typeid(*root).name());

        const auto& nodeNames = Config::GetSingleton().GetNodeNames();
        for (const auto& name : nodeNames) {
            auto* found = root->GetObjectByName(RE::BSFixedString(name));
            logger::debug("FindNodeName: {:08X} checking '{}' -> {}",
                         actor->GetFormID(), name, found ? "FOUND" : "not found");
            if (found) {
                return name;
            }
        }

        logger::debug("FindNodeName: {:08X} no matching node found out of {} candidates",
                     actor->GetFormID(), nodeNames.size());
        return "";
    }

    int NormalMapApplicator::DebugCyclePartition() {
        // Determine max partition from player's race
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) return -1;

        auto& cache = TextureCache::GetSingleton();

        // Get player state to know race
        std::string raceEditorID;
        if (auto* race = player->GetRace()) {
            raceEditorID = race->GetFormEditorID();
        }
        auto raceDir = cache.ResolveRaceDir(raceEditorID);
        auto* raceInfo = cache.GetRaceInfo(raceDir);
        int maxPartition = Config::GetSingleton().GetDynamicTextures().enabled ? 10 : (raceInfo ? raceInfo->maxPartition : 5);

        // Cycle: -1 → 1 → 2 → ... → maxPartition → -1
        // Skip 0 because partition 0 means "no override / use default"
        if (debugPartition_ < 1) {
            debugPartition_ = 1;
        } else if (debugPartition_ >= maxPartition) {
            debugPartition_ = -1;
        } else {
            debugPartition_++;
        }

        return debugPartition_;
    }

}  // namespace PNS



