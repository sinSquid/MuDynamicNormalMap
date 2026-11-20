#pragma once

namespace PNS {

    // Listens for SKSE ModCallbackEvent to handle "PNSUpdateRequest" from other mods
    class ModEventBridge : public RE::BSTEventSink<SKSE::ModCallbackEvent> {
    public:
        static ModEventBridge& GetSingleton() {
            static ModEventBridge instance;
            return instance;
        }

        static void Register();

        RE::BSEventNotifyControl ProcessEvent(
            const SKSE::ModCallbackEvent* event,
            RE::BSTEventSource<SKSE::ModCallbackEvent>*) override;

    private:
        ModEventBridge() = default;
    };

}  // namespace PNS
