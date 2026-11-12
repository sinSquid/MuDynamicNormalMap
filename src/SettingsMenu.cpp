#include "SettingsMenu.h"
#include "Config.h"
#include "RuntimeTuning.h"
#include "IniConfig.h"

namespace Mus::SettingsMenu {
    namespace {
        // ABI reference: ostrands/extern/SKSEMenuFramework/SKSEMenuFramework.h,
        // verified against installed SMF 3.18 exports. No linked ImGui/SMF runtime.
        struct Vec2 { float x, y; };
        using RenderFunction = void(__stdcall*)();
        struct MenuAPI {
            void (*addSection)(const char*, RenderFunction) = nullptr;
            void (*text)(const char*, const char*) = nullptr;
            bool (*inputInt)(const char*, int*, int, int, int) = nullptr;
            bool (*inputFloat)(const char*, float*, float, float, const char*, int) = nullptr;
            bool (*button)(const char*, Vec2) = nullptr;
            void (*beginDisabled)(bool) = nullptr;
            void (*endDisabled)() = nullptr;
        } api;

        struct Mailbox {
            std::mutex mutex;
            bool pending = false;
            std::uint64_t request = 0;
            std::string status;
        };
        const auto mailbox = std::make_shared<Mailbox>();
        RuntimeTuning draft{};
        bool initialized = false;
        bool registered = false;

        template<class T> bool Resolve(HMODULE module, const char* name, T& function) {
            function = reinterpret_cast<T>(GetProcAddress(module, name));
            return function != nullptr;
        }
        void Status(std::string message) {
            std::lock_guard lock(mailbox->mutex);
            mailbox->status = std::move(message);
        }
        void Text(const std::string& message) { api.text(message.c_str(), nullptr); }

        void QueueApply(const RuntimeTuning& tuning) {
            auto tasks = SKSE::GetTaskInterface();
            if (!tasks) {
                Status("暂时无法应用设置，请稍后重试。");
                return;
            }
            const auto state = mailbox;
            std::uint64_t request;
            {
                std::lock_guard lock(state->mutex);
                if (state->pending) return;
                request = ++state->request;
                state->pending = true;
                state->status = "等待应用；恢复游戏后生效。";
            }
            try {
                tasks->AddTask([state, request, tuning]() {
                    std::string error;
                    bool applied = false;
                    try {
                        applied = Config::GetSingleton().ApplyRuntimeTuning(tuning, error);
                    } catch (const std::exception& e) {
                        error = e.what();
                    } catch (...) {
                        error = "未知错误";
                    }
                    std::lock_guard lock(state->mutex);
                    if (state->request != request) return;
                    state->pending = false;
                    state->status = applied ? "已应用到当前游戏；此操作未写入 INI。" : "应用失败：" + error;
                    // A completion never rewrites the render thread's draft.
                });
            } catch (const std::exception& e) {
                std::lock_guard lock(state->mutex);
                state->pending = false;
                state->status = std::string("无法提交应用：") + e.what();
            } catch (...) {
                std::lock_guard lock(state->mutex);
                state->pending = false;
                state->status = "无法提交应用，请稍后重试。";
            }
        }

        void __stdcall Render() {
            const auto live = Config::GetSingleton().GetRuntimeTuning();
            if (!live) { Text("设置尚未准备好。"); return; }
            if (!initialized) { draft = *live; initialized = true; }
            bool pending;
            std::string status;
            {
                std::lock_guard lock(mailbox->mutex);
                pending = mailbox->pending;
                status = mailbox->status;
            }
            Text("动态法线 · 更新设置");
            Text("当前检测间隔：" + std::to_string(live->detectTickMS) + " 毫秒");
            Text("当前检测距离：" + std::to_string(live->detectDistance) + "；更新距离：" + std::to_string(live->updateDistance));
            Text("当前模型变更等待：" + std::to_string(live->updateDelayTick) + " 帧");
            Text("修改下方数值后点击应用；保存到 INI 只影响下次读取，不会立即应用。");
            api.beginDisabled(pending);
            api.inputInt("检测间隔（毫秒）", &draft.detectTickMS, 100, 1000, 0);
            api.inputFloat("检测距离（游戏单位）", &draft.detectDistance, 10.0f, 100.0f, "%.1f", 0);
            api.inputFloat("更新距离（游戏单位）", &draft.updateDistance, 10.0f, 100.0f, "%.1f", 0);
            api.inputInt("模型变更等待（帧）", &draft.updateDelayTick, 1, 10, 0);
            Text("距离为 0 表示不限；更新范围不会小于检测范围。");
            Text("等待值只影响后续模型变更；实际执行还需下一次更新帧。");
            RuntimeTuning normalized{};
            std::string error;
            const bool valid = NormalizeRuntimeTuning(draft, normalized, error);
            if (!valid) Text("请修正：" + error);
            else if (normalized.updateDistance != draft.updateDistance)
                Text("应用时更新距离将调整为：" + std::to_string(normalized.updateDistance));
            api.beginDisabled(!valid);
            const bool apply = api.button("应用到当前游戏", {0, 0});
            const bool save = api.button("保存到 INI", {0, 0});
            api.endDisabled();
            const bool undo = api.button("撤销修改", {0, 0});
            api.endDisabled();
            if (!pending && valid && apply) {
                draft = normalized;
                QueueApply(normalized);
            } else if (!pending && valid && save) {
                try {
                    const auto path = std::filesystem::path(GetRuntimeSKSEDirectory()) / "MuDynamicNormalMap.ini";
                    if (IniConfig::SaveRuntimeTuning(path, normalized, error))
                        Status("已保存到 INI；当前游戏设置没有改变。如需立即生效，请点击应用。");
                    else Status("保存失败：" + error);
                } catch (const std::exception& e) {
                    Status(std::string("保存失败：") + e.what());
                }
            } else if (!pending && undo) {
                draft = *live;
                Status("已恢复为当前游戏正在使用的数值。");
            }
            if (!status.empty()) Text(status);
        }
    }

    void Register() {
        if (registered) return;
        const auto module = GetModuleHandleW(L"SKSEMenuFramework");
        if (!module) {
            logger::info("SKSE Menu Framework unavailable; normal-map settings remain available in INI");
            return;
        }
        MenuAPI resolved;
        const bool ready = Resolve(module, "AddSectionItem", resolved.addSection) &&
            Resolve(module, "igTextUnformatted", resolved.text) &&
            Resolve(module, "igInputInt", resolved.inputInt) &&
            Resolve(module, "igInputFloat", resolved.inputFloat) &&
            Resolve(module, "igButton", resolved.button) &&
            Resolve(module, "igBeginDisabled", resolved.beginDisabled) &&
            Resolve(module, "igEndDisabled", resolved.endDisabled);
        if (!ready) {
            logger::warn("SKSE Menu Framework is missing a required menu export; optional menu skipped");
            return;
        }
        api = resolved;
        api.addSection("Mu Dynamic NormalMap/更新设置", Render);
        registered = true;
    }
}
