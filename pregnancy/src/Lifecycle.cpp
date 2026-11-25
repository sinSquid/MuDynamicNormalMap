#include "pch.h"
#include "Lifecycle.h"
#include "Rendering/MuBridge.h"
#include "Actor/ActorManager.h"
#include "Actor/NormalMapApplicator.h"
#include "Compat/BodyProviders.h"
#include "Compat/SBMCompat.h"
#include "Core/Config.h"
#include "Core/SKEEInterface.h"
#include "Core/TextureCache.h"
#include "Events/EquipEventSink.h"
#include "Events/ModEventBridge.h"
#include "Events/ObjectLoadSink.h"

namespace PNS {
    namespace {
        std::atomic<std::uint64_t> generation{0};
        std::atomic<bool> active{false};
        bool initialized = false;
        auto lastPoll = std::chrono::steady_clock::now();

        void InvalidateSession() {
            active.store(false);
            generation.fetch_add(1);
            if (initialized) ActorManager::GetSingleton().Clear();
            else ClearInstalledLayers();
        }

        class InputHandler final : public RE::BSTEventSink<RE::InputEvent*> {
        public:
            static InputHandler& GetSingleton() {
                static InputHandler instance;
                return instance;
            }
            RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* events,
                RE::BSTEventSource<RE::InputEvent*>*) override {
                const auto session = SessionGeneration();
                if (session != keySession || !ModuleEnabled() || !SessionActive()) {
                    heldKeys.clear();
                    keySession = session;
                }
                if (!ModuleEnabled() || !SessionActive() || !events)
                    return RE::BSEventNotifyControl::kContinue;
                const auto& keys = Config::GetSingleton().GetForceUpdateKey().keys;
                if (keys.empty()) return RE::BSEventNotifyControl::kContinue;
                bool triggered = false;
                for (auto* event = *events; event; event = event->next) {
                    auto* button = event->AsButtonEvent();
                    if (!button || button->GetDevice() != RE::INPUT_DEVICE::kKeyboard) continue;
                    if (button->IsUp()) heldKeys.erase(button->GetIDCode());
                    else if (button->IsPressed() || button->IsDown()) heldKeys.insert(button->GetIDCode());
                    if (button->GetIDCode() == keys.back() && button->IsDown() && !button->IsRepeating())
                        triggered = true;
                }
                if (triggered && std::all_of(keys.begin(), keys.end(), [&](auto key) { return heldKeys.contains(key); })) {
                    QueueSessionTask([] {
                        if (Config::GetSingleton().IsDebugMode()) {
                            const auto partition = NormalMapApplicator::DebugCyclePartition();
                            Notify(fmt::format("PNM++ Preview: {}", partition < 0 ? "AUTO" : std::to_string(partition)).c_str());
                        }
                        ActorManager::GetSingleton().ProcessAllLoaded();
                    });
                }
                return RE::BSEventNotifyControl::kContinue;
            }
        private:
            std::unordered_set<std::uint32_t> heldKeys;
            std::uint64_t keySession = 0;
        };
    }

    bool ModuleEnabled() { return Config::GetSingleton().IsModuleEnabled(); }
    bool SessionActive() { return active.load(); }
    std::uint64_t SessionGeneration() { return generation.load(); }

    void QueueSessionTask(std::function<void()> task) {
        const auto session = SessionGeneration();
        if (!ModuleEnabled() || !SessionActive()) return;
        if (auto* tasks = SKSE::GetTaskInterface()) {
            tasks->AddTask([session, task = std::move(task)] {
                if (ModuleEnabled() && SessionActive() && SessionGeneration() == session) task();
            });
        }
    }

    void HandleMessage(SKSE::MessagingInterface::Message* message) {
        if (!message) return;
        switch (message->type) {
        case SKSE::MessagingInterface::kPostPostLoad:
            Config::GetSingleton().Load();
            if (ModuleEnabled()) SKEEInterface::GetSingleton().AcquireInterfaces();
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            if (!ModuleEnabled() || initialized) break;
            SKEEInterface::GetSingleton().RetryIfNeeded();
            BodyProviders::Initialize();
            TextureCache::GetSingleton().ScanAll();
            SBMCompat::GetSingleton().Init();
            EquipEventSink::Register();
            ObjectLoadSink::Register();
            ModEventBridge::Register();
            if (auto* input = RE::BSInputDeviceManager::GetSingleton())
                input->AddEventSink(&InputHandler::GetSingleton());
            initialized = true;
            break;
        case SKSE::MessagingInterface::kPreLoadGame:
            InvalidateSession();
            break;
        case SKSE::MessagingInterface::kNewGame:
        case SKSE::MessagingInterface::kPostLoadGame:
            InvalidateSession();
            // kPostLoadGame carries success as the pointer value, not a bool*.
            if (message->type == SKSE::MessagingInterface::kPostLoadGame && !message->data) break;
            active.store(true);
            lastPoll = std::chrono::steady_clock::now();
            QueueSessionTask([] { ActorManager::GetSingleton().ProcessAllLoaded(); });
            break;
        default:
            break;
        }
    }

    void TickFrame() {
        if (!initialized || !ModuleEnabled() || !SessionActive()) return;
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player || !player->Is3DLoaded()) return;
        BodyProviders::Tick();
        const float interval = Config::GetSingleton().GetUpdateInterval();
        const auto now = std::chrono::steady_clock::now();
        if (interval > 0 && std::chrono::duration<float>(now - lastPoll).count() >= interval) {
            lastPoll = now;
            ActorManager::GetSingleton().PollUpdate();
        }
    }

    void Shutdown() {
        // The host can observe quit for more than one frame.
        if (active.exchange(false)) {
            generation.fetch_add(1);
            if (initialized) ActorManager::GetSingleton().Clear();
            else ClearInstalledLayers();
        }
    }
}
