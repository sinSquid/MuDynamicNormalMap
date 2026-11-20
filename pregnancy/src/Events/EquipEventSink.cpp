#include "pch.h"
#include "Events/EquipEventSink.h"
#include "Actor/ActorManager.h"
#include "Lifecycle.h"

namespace PNS {

    void EquipEventSink::Register() {
        auto* eventSource = RE::ScriptEventSourceHolder::GetSingleton();
        if (eventSource) {
            eventSource->AddEventSink(&GetSingleton());
            logger::info("EquipEventSink: Registered");
        }
    }

    RE::BSEventNotifyControl EquipEventSink::ProcessEvent(
        const RE::TESEquipEvent* event,
        RE::BSTEventSource<RE::TESEquipEvent>*) {

        if (!event || !ModuleEnabled() || !SessionActive()) return RE::BSEventNotifyControl::kContinue;

        // Check if the equipped/unequipped item has the body slot (slot 32 = 0x04)
        auto* form = RE::TESForm::LookupByID(event->baseObject);
        if (!form) return RE::BSEventNotifyControl::kContinue;

        auto* armor = form->As<RE::TESObjectARMO>();
        if (!armor) return RE::BSEventNotifyControl::kContinue;

        // Check if this armor uses the body slot (BipedObjectSlot::kBody = slot 32)
        if (!armor->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kBody)) {
            return RE::BSEventNotifyControl::kContinue;
        }

        // Get the actor
        auto actorRef = event->actor;
        if (!actorRef) return RE::BSEventNotifyControl::kContinue;

        auto* actor = actorRef->As<RE::Actor>();
        if (!actor) return RE::BSEventNotifyControl::kContinue;

        logger::debug("EquipEventSink: Body equipment {} for {:08X}",
                      event->equipped ? "equipped" : "unequipped", actor->GetFormID());

        // Equipment change may alter node names, so force update
        QueueSessionTask([id=actor->GetFormID()] {
            auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
            if (actor) ActorManager::GetSingleton().ProcessActor(actor,true);
        });

        return RE::BSEventNotifyControl::kContinue;
    }

}  // namespace PNS

