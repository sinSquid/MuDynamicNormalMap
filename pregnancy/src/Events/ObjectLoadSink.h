#pragma once

namespace PNS {

    class ObjectLoadSink : public RE::BSTEventSink<RE::TESObjectLoadedEvent> {
    public:
        static ObjectLoadSink& GetSingleton() {
            static ObjectLoadSink instance;
            return instance;
        }

        static void Register();

        RE::BSEventNotifyControl ProcessEvent(
            const RE::TESObjectLoadedEvent* event,
            RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override;

    private:
        ObjectLoadSink() = default;
    };

}  // namespace PNS
