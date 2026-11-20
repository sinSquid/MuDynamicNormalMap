#pragma once

namespace PNS {

    class EquipEventSink : public RE::BSTEventSink<RE::TESEquipEvent> {
    public:
        static EquipEventSink& GetSingleton() {
            static EquipEventSink instance;
            return instance;
        }

        static void Register();

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESEquipEvent* event,
            RE::BSTEventSource<RE::TESEquipEvent>*) override;

    private:
        EquipEventSink() = default;
    };

}  // namespace PNS
