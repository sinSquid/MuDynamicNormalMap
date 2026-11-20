#include "pch.h"
#include "Actor/ActorManager.h"
#include "Compat/BodyProviders.h"
#include "Compat/ProviderMorphs.h"
#include "Compat/OSkin.h"
#include "Core/SKEEInterface.h"
#include "Rendering/MuBridge.h"
#include "Lifecycle.h"

namespace PNS::BodyProviders {
    namespace {
        ProviderMorphs::Providers providers;
        std::unordered_map<RE::FormID, ProviderMorphs::Snapshot> snapshots;

        struct Pending {
            RE::ActorHandle handle;
            std::uint32_t slots;
            std::uint64_t due;
        };
        std::mutex pendingMutex;
        std::uint64_t frame = 0;
        std::unordered_map<RE::FormID, Pending> pending;

        struct Collector : IBodyMorphInterface::MorphValueVisitor {
            ProviderMorphs::Snapshot value;
            void Visit(RE::TESObjectREFR*, const char* slider, const char* key, float amount) override {
                value.Add(slider, key, amount, providers);
            }
        };
    }

    void Initialize() {
        OSkin::Initialize();
        if (auto* data = RE::TESDataHandler::GetSingleton()) {
            providers.obody = data->LookupModByName("OBody.esp") != nullptr;
            providers.abba = data->LookupModByName("ABBA.esp") != nullptr;
            providers.abm = data->LookupModByName("ArousedBodyMorphs.esp") != nullptr;
            providers.slif = providers.abba && data->LookupModByName("SexLab Inflation Framework.esp") != nullptr;
        }
        logger::info("Body compatibility: OBody={}, ABBA={}, Aroused BodyMorphs={}, ABBA/SLIF={}",
            providers.obody, providers.abba, providers.abm, providers.slif);
    }

    void Observe(RE::Actor* actor) {
        if (!ModuleEnabled() || !SessionActive()) return;
        OSkin::Observe(actor);
        if (!providers.Active() || !actor || !actor->Is3DLoaded()) return;
        auto* morphs = SKEEInterface::GetSingleton().GetBodyMorphInterface();
        if (!morphs) return;
        Collector collector;
        morphs->VisitMorphValues(actor, collector);
        const auto id = actor->GetFormID();
        auto it = snapshots.find(id);
        if (collector.value.Empty() && it == snapshots.end()) return;
        static const ProviderMorphs::Snapshot empty;
        const auto slots = collector.value.ChangedSlots(it == snapshots.end() ? empty : it->second);
        if (!slots) return;
        if (collector.value.Empty()) snapshots.erase(id);
        else snapshots.insert_or_assign(id, std::move(collector.value));
        Request(actor, slots);
    }

    void Request(RE::Actor* actor, std::uint32_t slots) {
        if (!ModuleEnabled() || !SessionActive() || !actor || !slots) return;
        std::lock_guard lock(pendingMutex);
        // Merge bursts without postponing an actor indefinitely during continuous updates.
        auto [it, added] = pending.try_emplace(actor->GetFormID(), Pending{
            actor->GetHandle(), slots, frame + std::max(1u, MuUpdateDelayFrames())});
        if (!added) it->second.slots |= slots;
    }

    void Tick() {
        if (!ModuleEnabled() || !SessionActive()) return;
        std::vector<Pending> ready;
        {
            std::lock_guard lock(pendingMutex);
            ++frame;
            std::erase_if(pending, [&](const auto& entry) {
                if (entry.second.due > frame) return false;
                ready.push_back(entry.second);
                return true;
            });
        }
        for (const auto& item : ready) {
            auto actor = item.handle.get();
            if (actor && actor->Is3DLoaded()) {
                ActorManager::GetSingleton().UpdatePregnancy(actor.get());
                QueueMuSlots(actor.get(), item.slots);
            }
        }
    }

    void Forget(RE::FormID actor) {
        OSkin::Forget(actor);
        snapshots.erase(actor);
        std::lock_guard lock(pendingMutex);
        pending.erase(actor);
    }
    void Clear() {
        OSkin::Clear();
        snapshots.clear();
        std::lock_guard lock(pendingMutex);
        pending.clear();
    }
}

