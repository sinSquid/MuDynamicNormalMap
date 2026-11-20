#include "pch.h"
#include "Events/ObjectLoadSink.h"
#include "Actor/ActorManager.h"
#include "Lifecycle.h"

namespace PNS {

    void ObjectLoadSink::Register() {
        auto* eventSource = RE::ScriptEventSourceHolder::GetSingleton();
        if (eventSource) {
            eventSource->AddEventSink(&GetSingleton());
            logger::info("ObjectLoadSink: Registered");
        }
    }

    RE::BSEventNotifyControl ObjectLoadSink::ProcessEvent(
        const RE::TESObjectLoadedEvent* event,
        RE::BSTEventSource<RE::TESObjectLoadedEvent>*) {

        if (!event || !ModuleEnabled() || !SessionActive()) return RE::BSEventNotifyControl::kContinue;

        // Only care about loaded objects (not unloaded)
        if (!event->loaded) {
            // Actor unloaded — remove from tracking
            QueueSessionTask([id=event->formID] { ActorManager::GetSingleton().RemoveActor(id); });
            return RE::BSEventNotifyControl::kContinue;
        }

        // Check if loaded object is an actor
        auto* form = RE::TESForm::LookupByID(event->formID);
        if (!form) return RE::BSEventNotifyControl::kContinue;

        auto* actor = form->As<RE::Actor>();
        if (!actor) return RE::BSEventNotifyControl::kContinue;

        logger::debug("ObjectLoadSink: Actor {:08X} loaded", actor->GetFormID());

        // Process after a small delay via task to ensure 3D is fully loaded
        QueueSessionTask([formID = actor->GetFormID()]() {
            auto* form = RE::TESForm::LookupByID(formID);
            if (!form) return;
            auto* actor = form->As<RE::Actor>();
            if (!actor || !actor->Is3DLoaded()) return;
            ActorManager::GetSingleton().ProcessActor(actor, true);
        });

        return RE::BSEventNotifyControl::kContinue;
    }

}  // namespace PNS

