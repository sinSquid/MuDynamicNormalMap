#include "SettingsMenu.h"
#include "Config.h"
#include "RuntimeTuning.h"
#include "SettingsCatalog.h"
#include "IniConfig.h"
#include "TaskManager.h"

namespace Mus::SettingsMenu {
    namespace {
        // cimgui ABI: ostrands/extern/SKSEMenuFramework/SKSEMenuFramework.h.
        // All exports used by the optional menu are resolved before registration.
        struct Vec2 { float x, y; };
        using RenderFunction = void(__stdcall*)();
        struct MenuAPI {
            void (*addSection)(const char*, RenderFunction) = nullptr;
            void (*text)(const char*, const char*) = nullptr;
            bool (*inputInt)(const char*, int*, int, int, int) = nullptr;
            bool (*inputFloat)(const char*, float*, float, float, const char*, int) = nullptr;
            bool (*inputText)(const char*, char*, std::size_t, int, int (*)(void*), void*) = nullptr;
            bool (*checkbox)(const char*, bool*) = nullptr;
            bool (*combo)(const char*, int*, const char* const*, int, int) = nullptr;
            bool (*header)(const char*, int) = nullptr;
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

        const SettingDescriptor* ByKey(std::string_view key) {
            for (const auto& setting : GetSettingsCatalog()) if (key == setting.key) return &setting;
            return nullptr;
        }
        int DraftInt(std::string_view key) {
            const auto setting = ByKey(key);
            return setting ? std::get<int>(setting->read(draft)) : 0;
        }
        bool DraftBool(std::string_view key) {
            const auto setting = ByKey(key);
            return setting && std::get<bool>(setting->read(draft));
        }
        bool HasHybridCores() {
            // EfficiencyClass is meaningful only if the OS reports different core classes.
            static const bool hybrid = [] {
                DWORD bytes = 0;
                GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bytes);
                if (!bytes) return false;
                std::vector<std::uint64_t> storage((bytes + 7) / 8);
                auto data = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(storage.data());
                if (!GetLogicalProcessorInformationEx(RelationProcessorCore, data, &bytes)) return false;
                int firstClass = -1;
                for (DWORD offset = 0; offset < bytes;) {
                    const auto core = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(reinterpret_cast<char*>(data) + offset);
                    if (!core->Size || core->Size > bytes - offset) break;
                    if (core->Relationship == RelationProcessorCore) {
                        const int efficiency = core->Processor.EfficiencyClass;
                        if (firstClass >= 0 && firstClass != efficiency) return true;
                        firstClass = efficiency;
                    }
                    offset += core->Size;
                }
                return false;
            }();
            return hybrid;
        }
        std::string DisabledReason(const SettingDescriptor& setting) {
            const std::string_view key = setting.key;
            if (key == "TaskQTickMS" && DraftInt("AutoTaskQ") <= 3)
                return "当前调度模式使用直接队列，此间隔不参与限速；模式 4 / 5 才使用间隔。";
            if (key == "EcoreUse") {
                if (!HasHybridCores()) return "系统未报告不同性能等级的 CPU 核心；此选项仅适用于混合架构。";
                if (DraftInt("AutoTaskQ") == 0) return "立即模式固定使用性能核心。";
            }
            if (key == "RealtimeDetectHead" && !DraftBool("HeadEnable"))
                return "全局头部处理已关闭；头部检测设置当前不起作用。";
            if (key == "GPUForceSync" && !Config::GetSingleton().GetRuntimeTuning()->GPUEnable)
                return "GPU 计算已关闭；此项在启用 GPU 后生效。";
            if ((key == "DiskCacheLimitMB" || key == "ClearDiskCache") && !DraftBool("DiskCache"))
                return "磁盘缓存已关闭。";
            return {};
        }
        const char* PolicyLabel(ApplyPolicy policy) {
            switch (policy) {
            case ApplyPolicy::Immediate: return "应用后生效";
            case ApplyPolicy::IdleReconfigure: return "空闲后重配";
            case ApplyPolicy::Regenerate: return "重建法线";
            case ApplyPolicy::Restart: return "保存后重启生效";
            }
            return "";
        }
        std::string KeyName(int code) {
            switch (code) {
            case 0: return "无 / 主键禁用";
            case 1: return "Esc";
            case 12: return "-";
            case 13: return "=";
            case 14: return "Backspace";
            case 15: return "Tab";
            case 26: return "[";
            case 27: return "]";
            case 28: return "Enter";
            case 29: return "左 Ctrl";
            case 39: return ";";
            case 40: return "'";
            case 41: return "`";
            case 42: return "左 Shift";
            case 43: return "反斜杠";
            case 51: return ",";
            case 52: return ".";
            case 53: return "/";
            case 54: return "右 Shift";
            case 55: return "小键盘 *";
            case 56: return "左 Alt";
            case 57: return "空格";
            case 58: return "Caps Lock";
            case 69: return "Num Lock";
            case 70: return "Scroll Lock";
            case 71: return "小键盘 7";
            case 72: return "小键盘 8";
            case 73: return "小键盘 9";
            case 74: return "小键盘 -";
            case 75: return "小键盘 4";
            case 76: return "小键盘 5";
            case 77: return "小键盘 6";
            case 78: return "小键盘 +";
            case 79: return "小键盘 1";
            case 80: return "小键盘 2";
            case 81: return "小键盘 3";
            case 82: return "小键盘 0";
            case 83: return "小键盘小数点";
            case 87: return "F11";
            case 88: return "F12";
            case 156: return "小键盘 Enter";
            case 157: return "右 Ctrl";
            case 181: return "小键盘 /";
            case 184: return "右 Alt";
            case 199: return "Home";
            case 200: return "↑";
            case 201: return "Page Up";
            case 203: return "←";
            case 205: return "→";
            case 207: return "End";
            case 208: return "↓";
            case 209: return "Page Down";
            case 210: return "Insert";
            case 211: return "Delete";
            case 219: return "左 Windows";
            case 220: return "右 Windows";
            case 221: return "菜单键";
            case 256: return "鼠标左键";
            case 257: return "鼠标右键";
            case 258: return "鼠标中键";
            case 264: return "滚轮上";
            case 265: return "滚轮下";
            }
            if (code >= 2 && code <= 10) return std::to_string(code - 1);
            if (code == 11) return "0";
            if (code >= 59 && code <= 68) return "F" + std::to_string(code - 58);
            constexpr const char* letters = "QWERTYUIOPASDFGHJKLZXCVBNM";
            constexpr int scans[] = {16,17,18,19,20,21,22,23,24,25,30,31,32,33,34,35,36,37,38,44,45,46,47,48,49,50};
            for (std::size_t i = 0; i < std::size(scans); ++i) if (code == scans[i]) return std::string(1, letters[i]);
            if (code >= 259 && code <= 263) return "鼠标按钮 " + std::to_string(code - 255);
            constexpr const char* gamepad[] = {"十字键上","十字键下","十字键左","十字键右","Start","Back","左摇杆按下","右摇杆按下","LB","RB","A","B","X","Y","LT","RT"};
            if (code >= 266 && code <= 281) return std::string("手柄 ") + gamepad[code - 266];
            return "键码 " + std::to_string(code);
        }
        std::string DisplayValue(const SettingDescriptor& setting, const RuntimeTuning& values) {
            const auto value = setting.read(values);
            if (const auto toggle = std::get_if<bool>(&value)) return *toggle ? "开启" : "关闭";
            const std::string_view key = setting.key;
            if (key == "HotKey1" || key == "HotKey2") {
                const auto code = std::get<int>(value);
                return KeyName(code) + "（" + std::to_string(code) + "）";
            }
            if (setting.type == SettingType::Choice) {
                const auto choice = std::get<int>(value);
                std::istringstream stream(setting.options ? setting.options : "");
                int index = static_cast<int>(setting.minimum);
                for (std::string item; std::getline(stream, item, '|'); ++index)
                    if (index == choice) return item + "（" + std::to_string(choice) + "）";
            }
            return SettingValueText(setting, values);
        }
        void DrawEditor(const SettingDescriptor& setting) {
            const auto label = std::string(setting.label) + "###" + setting.section + "." + setting.key;
            auto value = setting.read(draft);
            if (std::string_view(setting.key) == "HotKey1") {
                constexpr int codes[] = {0,29,42,56,157,54,184};
                const char* names[] = {"无修饰键","左 Ctrl","左 Shift","左 Alt","右 Ctrl","右 Shift","右 Alt","自定义（下方输入键码）"};
                int selected = 7;
                for (int i = 0; i < 7; ++i) if (std::get<int>(value) == codes[i]) selected = i;
                if (api.combo("常用修饰键###modifier", &selected, names, 8, -1) && selected < 7) {
                    value = codes[selected];
                    setting.assign(draft, value);
                }
            }
            switch (setting.type) {
            case SettingType::Boolean: {
                auto next = std::get<bool>(value);
                if (api.checkbox(label.c_str(), &next)) setting.assign(draft, next);
                break;
            }
            case SettingType::Integer: {
                auto next = std::get<int>(value);
                if (api.inputInt(label.c_str(), &next, 1, 10, 0)) setting.assign(draft, next);
                break;
            }
            case SettingType::Float: {
                auto next = std::get<float>(value);
                if (api.inputFloat(label.c_str(), &next, 0.01f, 1.0f, "%.4f", 0)) setting.assign(draft, next);
                break;
            }
            case SettingType::String: {
                const auto& current = std::get<std::string>(value);
                std::vector<char> buffer(std::max<std::size_t>(4096, current.size() + 1024), '\0');
                std::copy(current.begin(), current.end(), buffer.begin());
                if (api.inputText(label.c_str(), buffer.data(), buffer.size(), 0, nullptr, nullptr))
                    setting.assign(draft, std::string(buffer.data()));
                break;
            }
            case SettingType::Choice: {
                std::vector<std::string> choices;
                std::istringstream stream(setting.options ? setting.options : "");
                for (std::string item; std::getline(stream, item, '|');) choices.push_back(std::move(item));
                std::vector<const char*> items;
                for (const auto& item : choices) items.push_back(item.c_str());
                const int offset = static_cast<int>(setting.minimum);
                const int current = std::get<int>(value);
                int selected = current - offset;
                if (api.combo(label.c_str(), &selected, items.data(), static_cast<int>(items.size()), -1))
                    setting.assign(draft, selected + offset);
                break;
            }
            }
            if (std::string_view(setting.key) == "NormalSmoothDegree")
                Text(DraftBool("AllowInvertNormalSmooth") ? "草稿允许反向法线；有效范围：0–90 度。" : "有效范围：0–180 度。");
            if (std::string_view(setting.key) == "HotKey1" || std::string_view(setting.key) == "HotKey2") {
                const auto code = std::get<int>(setting.read(draft));
                Text("草稿按键：" + KeyName(code) + "；SKSE 键码 / 扫描码：" + std::to_string(code));
            }
        }

        void QueueApply(const RuntimeTuning& tuning) {
            auto tasks = SKSE::GetTaskInterface();
            if (!tasks) { Status("暂时无法应用设置，请稍后重试。"); return; }
            const auto state = mailbox;
            std::uint64_t request;
            {
                std::lock_guard lock(state->mutex);
                if (state->pending) return;
                request = ++state->request;
                state->pending = true;
                state->status = "等待工作队列空闲；请恢复游戏。应用不会写入 INI。";
            }
            try {
                tasks->AddTask([state, request, tuning]() {
                    const auto complete = [state, request](bool success, const std::string& result) {
                        std::lock_guard lock(state->mutex);
                        if (state->request != request) return;
                        state->pending = false;
                        state->status = success ? result : "应用失败：" + result;
                        // Completion never rewrites the render thread's draft.
                    };
                    try { TaskManager::GetSingleton().QueueMenuApply(tuning, complete); }
                    catch (const std::exception& e) { complete(false, e.what()); }
                    catch (...) { complete(false, "未知错误"); }
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

        const char* Group(const SettingDescriptor& setting) {
            const std::string_view section = setting.section;
            if (section == "General") {
                const std::string_view key = setting.key;
                return key == "HotKey1" || key == "HotKey2" || key == "UseConsoleRef" ? "快捷键" : "常规";
            }
            if (section == "Performance") return "性能";
            if (section == "RealtimeDetect") return "实时检测";
            if (section == "Geometry") return "几何处理";
            if (section == "Texture") return "纹理";
            if (section == "Debug") return "调试";
            return setting.section;
        }
        enum class Page { Common, Performance, Quality };
        bool OnPage(std::string_view group, Page page) {
            if (page == Page::Common) return group == "常规" || group == "快捷键" || group == "实时检测";
            if (page == Page::Performance) return group == "性能";
            return group == "纹理" || group == "几何处理" || group == "调试";
        }
        void RenderPage(Page page) {
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
            // Use SMF's configured primary font (Chinese glyphs), without replacing its font stack.
            Text("Mu 动态法线 · 设置");
            Text("编辑的是草稿。应用：更新当前游戏；保存：只写 INI；撤销：恢复为当前实际值。");
            Text("三个页面共享同一份草稿；下方操作作用于所有页面的菜单设置。");
            api.beginDisabled(pending);
            constexpr const char* groups[] = {"常规","快捷键","实时检测","性能","纹理","几何处理","调试"};
            for (const auto group : groups) {
                if (!OnPage(group, page)) continue;
                std::size_t changed = 0;
                for (const auto& setting : GetMenuSettingsCatalog())
                    if (std::string_view(Group(setting)) == group && setting.read(draft) != setting.read(*live)) ++changed;
                const auto heading = std::string(group) + (changed ? "（" + std::to_string(changed) + " 项未应用）" : "") + "###group." + group;
                const bool advanced = std::string_view(group) == "几何处理" || std::string_view(group) == "调试";
                if (!api.header(heading.c_str(), advanced ? 0 : 1 << 5)) continue; // ImGuiTreeNodeFlags_DefaultOpen
                if (std::string_view(group) == "快捷键")
                    Text("选择修饰键并输入主键 SKSE 键码；不支持按键录制。修饰键 0 = 无，主键 0 = 禁用。小键盘小数点是 83，左 Shift 是 42。");
                if (std::string_view(group) == "常规")
                    Text("Condition 可进一步禁用某角色头部，无法绕过全局头部关闭；细节强度可被 Condition 角色规则覆盖。");
                if (std::string_view(group) == "纹理")
                    Text("最终细节强度可能由 Condition 的角色规则覆盖；此处调整全局默认值。");
                if (std::string_view(group) == "几何处理")
                    Text("高级画质设置：只在需要调整接缝或平滑效果时修改；应用会重新生成法线。");
                for (const auto& setting : GetMenuSettingsCatalog()) {
                    if (std::string_view(Group(setting)) != group) continue;
                    const auto disabled = DisabledReason(setting);
                    api.beginDisabled(!disabled.empty());
                    DrawEditor(setting);
                    api.endDisabled();
                    const auto changedRow = setting.read(draft) != setting.read(*live);
                    Text("当前：" + DisplayValue(setting, *live) + "；" + PolicyLabel(setting.policy) + (changedRow ? " · 未应用" : ""));
                    if (setting.help && *setting.help) Text(setting.help);
                    if (!disabled.empty()) Text("草稿条件：" + disabled);
                }
            }
            RuntimeTuning normalized{};
            // Only menu fields may be submitted; preserve hidden settings from the current snapshot.
            auto submitted = *live;
            for (const auto& setting : GetMenuSettingsCatalog()) setting.assign(submitted, setting.read(draft));
            std::string error;
            const bool valid = NormalizeRuntimeTuning(submitted, normalized, error);
            std::size_t changed = 0, restart = 0;
            for (const auto& setting : GetMenuSettingsCatalog()) {
                if (setting.read(draft) == setting.read(*live)) continue;
                ++changed;
                if (setting.policy == ApplyPolicy::Restart) ++restart;
            }
            Text("全部页面的草稿与当前游戏相差 " + std::to_string(changed) + " 项。");
            if (restart) Text("其中 " + std::to_string(restart) + " 项需要保存后重启。");
            if (!valid) Text("请修正：" + error);
            else if (normalized.updateDistance != draft.updateDistance)
                Text("应用或保存时更新距离将调整为：" + std::to_string(normalized.updateDistance));
            api.beginDisabled(!valid);
            const bool apply = api.button("应用到当前游戏", {0, 0});
            const bool save = api.button("保存到 INI", {0, 0});
            api.endDisabled();
            const bool undo = api.button("撤销修改", {0, 0});
            api.endDisabled();
            if (!pending && valid && apply) {
                draft = normalized;
                if (!changed) Status("草稿与当前游戏相同，无需应用。");
                else if (changed == restart) Status("仅修改了 " + std::to_string(restart) + " 项重启设置；请保存到 INI 并重启游戏，当前游戏未改变。");
                else QueueApply(normalized);
            } else if (!pending && valid && save) {
                try {
                    const auto path = std::filesystem::path(GetRuntimeSKSEDirectory()) / "MuDynamicNormalMap_User.ini";
                    if (IniConfig::SaveRuntimeTuning(path, normalized, error)) {
                        draft = normalized;
                        logger::info("Saved menu overrides to {}", path.string());
                        Status("已保存到 MuDynamicNormalMap_User.ini；当前游戏未改变。如需立即生效，请点击应用。");
                    } else {
                        logger::error("Menu INI save failed at {}: {}", path.string(), error);
                        Status("保存失败：" + error);
                    }
                } catch (const std::exception& e) {
                    logger::error("Menu INI save exception: {}", e.what());
                    Status(std::string("保存失败：") + e.what());
                }
            } else if (!pending && undo) {
                draft = *live;
                Status("已恢复为当前游戏正在使用的数值。");
            }
            {
                std::lock_guard lock(mailbox->mutex);
                status = mailbox->status;
            }
            if (!status.empty()) Text(status);
        }
        void __stdcall RenderCommon() { RenderPage(Page::Common); }
        void __stdcall RenderPerformance() { RenderPage(Page::Performance); }
        void __stdcall RenderQuality() { RenderPage(Page::Quality); }
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
            Resolve(module, "igInputText", resolved.inputText) &&
            Resolve(module, "igCheckbox", resolved.checkbox) &&
            Resolve(module, "igCombo_Str_arr", resolved.combo) &&
            Resolve(module, "igCollapsingHeader_TreeNodeFlags", resolved.header) &&
            Resolve(module, "igButton", resolved.button) &&
            Resolve(module, "igBeginDisabled", resolved.beginDisabled) &&
            Resolve(module, "igEndDisabled", resolved.endDisabled);
        if (!ready) {
            logger::warn("SKSE Menu Framework is missing a required menu export; optional menu skipped");
            return;
        }
        api = resolved;
        api.addSection("Mu Dynamic NormalMap/常用", RenderCommon);
        api.addSection("Mu Dynamic NormalMap/性能", RenderPerformance);
        api.addSection("Mu Dynamic NormalMap/画质与高级", RenderQuality);
        registered = true;
    }
}
