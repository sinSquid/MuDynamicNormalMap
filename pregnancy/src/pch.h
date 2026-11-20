#pragma once

#include <RE/Skyrim.h>
#include <REL/Relocation.h>
#include <SKSE/SKSE.h>

#include <nlohmann/json.hpp>
#include <spdlog/sinks/basic_file_sink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std::literals;
namespace fs = std::filesystem;
using json = nlohmann::json;


namespace logger = SKSE::log;

namespace PNS {
    inline constexpr auto ConfigDirectory = "PregnancyNormalMapPlusPlus";
    inline void Notify(const char* message) {
        auto* queue = RE::UIMessageQueue::GetSingleton();
        if (!queue) return;
        auto* data = static_cast<RE::HUDData*>(queue->CreateUIMessageData(RE::BSFixedString(RE::HUDData::CLASS_NAME)));
        if (!data) return;
        data->type = RE::HUD_MESSAGE_TYPE::kNotification;
        data->text = message;
        queue->AddMessage(RE::BSFixedString(RE::HUDMenu::MENU_NAME),RE::UI_MESSAGE_TYPE::kUpdate,data);
    }
}

