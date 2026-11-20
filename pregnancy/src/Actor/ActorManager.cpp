#include "pch.h"
#include "Actor/ActorManager.h"
#include "Actor/NormalMapApplicator.h"
#include "Core/Config.h"
#include "Core/SKEEInterface.h"
#include "Core/TextureCache.h"
#include "Compat/BodyProviders.h"
#include "Lifecycle.h"
#include "Rendering/MuBridge.h"
#include "RestoreQueue.h"

namespace PNS {

    std::pair<float, std::string> ActorManager::GetPregnancyLayer(RE::FormID actor) {
        if (!ModuleEnabled() || !SessionActive()) return {};
        // Rendering may run before the next actor poll after a filter change.
        if (!ShouldProcess(RE::TESForm::LookupByID<RE::Actor>(actor))) return {};
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
        if (!ModuleEnabled() || !SessionActive() || !actor || !actor->Is3DLoaded()) return;

        const bool eligible = ShouldProcess(actor);
        const bool installedLayer = HasInstalledLayer(actor->GetFormID());
        std::lock_guard lock(mutex_);
        if (!eligible) {
            auto found = trackedActors_.find(actor->GetFormID());
            if (found == trackedActors_.end() && installedLayer) {
                GetOrCreateState(actor);
                found = trackedActors_.find(actor->GetFormID());
            }
            if (found != trackedActors_.end()) {
                found->second.SuppressLayer(installedLayer);
                QueueRestore(actor->GetFormID(), found->second);
            }
            return;
        }
        auto& state = GetOrCreateState(actor);
        // Eligibility alone cannot complete physical restoration. In particular,
        // Mu's normal queue can report a filtered no-op as success. Finish the
        // independent restore before submitting a replacement layer.
        if (state.restorePending) {
            QueueRestore(actor->GetFormID(), state);
            return;
        }
        NormalMapApplicator::Apply(actor, state, forceUpdate, installedLayer);
        // Ordinary zero morphs and unavailable legacy stage textures use the
        // same physical restore path as exclusion, independent of Mu categories.
        if (state.restorePending) QueueRestore(actor->GetFormID(), state);
    }

    void ActorManager::QueueRestore(RE::FormID id, ActorState& state) {
        if (!state.restorePending || state.restoreQueued) return;
        const auto session = SessionGeneration();
        const auto request = ++nextRestoreRequest_;
        state.ScheduleRestore(request, [this, id, session, request] {
            return QueueLayerRestore([this, id, session, request] { RunRestore(id, session, request); });
        });
    }

    void ActorManager::RunRestore(RE::FormID id, std::uint64_t session, std::uint64_t request) {
        if (!ModuleEnabled() || !SessionActive() || session != SessionGeneration()) return;
        {
            std::lock_guard lock(mutex_);
            const auto found = trackedActors_.find(id);
            if (found == trackedActors_.end() || !found->second.TakeRestore(request)) return;
        }
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
        if (!actor || !actor->Is3DLoaded()) {
            RemoveActor(id);
            return;
        }
        // No actor-state mutex is held while restoring renderer-owned resources.
        bool restored = false;
        try { restored = RestoreInstalledLayer(actor); }
        catch (const std::exception& error) {
            logger::warn("Pregnancy restoration deferred for {:08X}: {}", id, error.what());
        }
        catch (...) { logger::warn("Pregnancy restoration deferred for {:08X}", id); }
        {
            std::lock_guard lock(mutex_);
            const auto found = trackedActors_.find(id);
            if (found == trackedActors_.end() || !found->second.FinishRestore(request, restored)) return;
            if (!restored) {
                QueueRestore(id, found->second);
                return;
            }
        }
        // Re-evaluate only after the old physical layer is gone. A category-
        // filtered/no-op new request now cannot strand that old layer.
        if (session == SessionGeneration() && ShouldProcess(actor)) UpdatePregnancy(actor, true);
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
        BodyProviders::Clear();
        ClearInstalledLayers();
        std::lock_guard lock(mutex_);
        trackedActors_.clear();
        logger::debug("ActorManager: Cleared all tracked actors");
    }

    void ActorManager::RemoveActor(RE::FormID formID) {
        BodyProviders::Forget(formID);
        ForgetInstalledLayer(formID);
        std::lock_guard lock(mutex_);
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
        std::vector<RE::FormID> unloaded;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [id, state] : trackedActors_) {
                auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
                if (!actor || !actor->Is3DLoaded()) unloaded.push_back(id);
            }
        }
        for (const auto id : unloaded) RemoveActor(id);
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


