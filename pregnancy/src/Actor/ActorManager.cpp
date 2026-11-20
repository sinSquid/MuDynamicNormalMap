#include "pch.h"
#include "Actor/ActorManager.h"
#include "Actor/NormalMapApplicator.h"
#include "Core/Config.h"
#include "Core/SKEEInterface.h"
#include "Core/TextureCache.h"
#include "Compat/BodyProviders.h"
#include "Lifecycle.h"

namespace PNS {

    std::pair<float, std::string> ActorManager::GetPregnancyLayer(RE::FormID actor) {
        if (!ModuleEnabled() || !SessionActive()) return {};
        std::lock_guard lock(mutex_);
        auto it = trackedActors_.find(actor);
        if (it == trackedActors_.end()) return {};
        return {it->second.currentMorph, it->second.currentTexture};
    }

    void ActorManager::ProcessActor(RE::Actor* actor, bool forceUpdate) {
        if (!ModuleEnabled() || !SessionActive()) return;
        BodyProviders::Observe(actor);
        UpdatePregnancy(actor, forceUpdate);
    }

    void ActorManager::UpdatePregnancy(RE::Actor* actor, bool forceUpdate) {
        if (!ModuleEnabled() || !SessionActive() || !actor || !ShouldProcess(actor)) return;

        std::lock_guard lock(mutex_);
        auto& state = GetOrCreateState(actor);
        NormalMapApplicator::Apply(actor, state, forceUpdate);
    }

    void ActorManager::ProcessAllLoaded() {
        if (!ModuleEnabled() || !SessionActive()) return;
        logger::debug("ActorManager: Processing all loaded actors...");

        auto& skee = PNS::SKEEInterface::GetSingleton();
        logger::debug("ActorManager: SKEE available={} (bodyMorph={:p} override={:p})",
                     skee.IsAvailable(),
                     (void*)skee.GetBodyMorphInterface(),
                     (void*)skee.GetOverrideInterface());

        auto* processLists = RE::ProcessLists::GetSingleton();
        if (!processLists) {
            logger::debug("ActorManager: ProcessLists is null");
            return;
        }

        logger::debug("ActorManager: highActorHandles={}", processLists->highActorHandles.size());

        int count = 0;

        // Process high actors (fully loaded with 3D)
        for (auto& handle : processLists->highActorHandles) {
            auto actor = handle.get();
            if (actor && actor.get()) {
                ProcessActor(actor.get(), true);
                count++;
            }
        }

        // Also process the player
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            ProcessActor(player, true);
            count++;
        }

        logger::debug("ActorManager: Processed {} actors", count);
    }

    void ActorManager::Clear() {
        std::lock_guard lock(mutex_);
        BodyProviders::Clear();
        trackedActors_.clear();
        logger::debug("ActorManager: Cleared all tracked actors");
    }

    void ActorManager::RemoveActor(RE::FormID formID) {
        std::lock_guard lock(mutex_);
        BodyProviders::Forget(formID);
        trackedActors_.erase(formID);
    }

    void ActorManager::PollUpdate() {
        if (!ModuleEnabled() || !SessionActive()) return;
        auto* processLists = RE::ProcessLists::GetSingleton();
        if (!processLists) return;

        for (auto& handle : processLists->highActorHandles) {
            auto actor = handle.get();
            if (actor && actor.get()) {
                ProcessActor(actor.get());
            }
        }

        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
            ProcessActor(player);
        }
        std::lock_guard lock(mutex_);
        std::erase_if(trackedActors_,[](const auto& entry) {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(entry.first);
            return !actor || !actor->Is3DLoaded();
        });
    }

    ActorState& ActorManager::GetOrCreateState(RE::Actor* actor) {
        auto formID = actor->GetFormID();
        auto it = trackedActors_.find(formID);
        if (it != trackedActors_.end()) {
            return it->second;
        }

        auto& state = trackedActors_[formID];
        state.formID = formID;
        state.isFemale = actor->GetActorBase() && actor->GetActorBase()->GetSex() == RE::SEX::kFemale;

        // Cache race editor ID
        if (auto* race = actor->GetRace()) {
            state.raceEditorID = race->GetFormEditorID();
        }

        // Find NiOverride node name
        state.nodeName = NormalMapApplicator::FindNodeName(actor);

        return state;
    }

    bool ActorManager::ShouldProcess(RE::Actor* actor) const {
        if (!actor) return false;
        if (!actor->Is3DLoaded()) {
            return false;
        }
        if (!actor->HasKeywordString("ActorTypeNPC")) {
            logger::debug("ShouldProcess: {:08X} not ActorTypeNPC, skipping", actor->GetFormID());
            return false;
        }

        auto& config = Config::GetSingleton();
        auto& filter = config.GetActorFilter();

        // Skip player if disabled
        if (actor == RE::PlayerCharacter::GetSingleton() && !config.IsPlayerEnabled()) {
            logger::debug("ShouldProcess: {:08X} player disabled, skipping", actor->GetFormID());
            return false;
        }

        // Exclude children
        if (filter.excludeChildren) {
            if (actor->HasKeywordString("ActorTypeChild")) {
                logger::debug("ShouldProcess: {:08X} is child, skipping", actor->GetFormID());
                return false;
            }
        }

        // Gender filter
        if (filter.requireFemale) {
            auto* base = actor->GetActorBase();
            if (base && base->GetSex() != RE::SEX::kFemale) {
                logger::debug("ShouldProcess: {:08X} not female (sex={}), skipping",
                             actor->GetFormID(), static_cast<int>(base->GetSex()));
                return false;
            }
        }

        // Keyword exclusion
        for (const auto& kwStr : filter.excludeKeywords) {
            if (actor->HasKeywordString(kwStr.c_str())) {
                logger::debug("ShouldProcess: {:08X} excluded by keyword '{}'", actor->GetFormID(), kwStr);
                return false;
            }
        }

        logger::debug("ShouldProcess: {:08X} passed all filters", actor->GetFormID());
        return true;
    }

}  // namespace PNS


