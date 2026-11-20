#include "pch.h"
#include "Events/ModEventBridge.h"
#include "Actor/ActorManager.h"
#include "Compat/BodyProviders.h"
#include "Compat/ProviderMorphs.h"
#include "Lifecycle.h"

namespace PNS {

    void ModEventBridge::Register() {
        auto* modCallback = SKSE::GetModCallbackEventSource();
        if (modCallback) {
            modCallback->AddEventSink(&GetSingleton());
            logger::info("ModEventBridge: Registered for ModCallbackEvent");
        }
    }

    RE::BSEventNotifyControl ModEventBridge::ProcessEvent(
        const SKSE::ModCallbackEvent* event,
        RE::BSTEventSource<SKSE::ModCallbackEvent>*) {

        if (!event || !ModuleEnabled() || !SessionActive()) return RE::BSEventNotifyControl::kContinue;

        if (event->eventName == "Obody_ApplyMorph" && event->sender) {
            const auto id = event->sender->GetFormID();
            QueueSessionTask([id] {
                BodyProviders::Request(RE::TESForm::LookupByID<RE::Actor>(id), ProviderMorphs::allSlots);
            });
        }
        if (event->eventName == "PNSUpdateRequest") {
            logger::debug("ModEventBridge: Received PNSUpdateRequest");

            auto* sender = event->sender;
            if (sender) {
                // Specific actor requested
                auto* actor = sender->As<RE::Actor>();
                if (actor) {
                    QueueSessionTask([id=actor->GetFormID()] {
                        if (auto* actor = RE::TESForm::LookupByID<RE::Actor>(id)) ActorManager::GetSingleton().ProcessActor(actor,true);
                    });
                }
            } else {
                // No specific actor — update all
                QueueSessionTask([] { ActorManager::GetSingleton().ProcessAllLoaded(); });
            }
        }

        return RE::BSEventNotifyControl::kContinue;
    }

}  // namespace PNS

