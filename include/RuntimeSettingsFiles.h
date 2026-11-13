#pragma once
#include "SettingsCatalog.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Mus {
    inline bool LoadRuntimeSettingsFiles(const std::filesystem::path& defaultsPath,
        const std::filesystem::path& userPath, const RuntimeTuning& current,
        RuntimeTuning& result, std::string& error, bool loggingOnly = false) {
        std::ifstream defaults(defaultsPath, std::ios::binary);
        if (!defaults) { error = "Cannot open default INI: " + defaultsPath.string(); return false; }
        std::string defaultText{std::istreambuf_iterator<char>(defaults), {}};
        if (defaults.bad() || defaultText.find('\0') != std::string::npos ||
            defaultText.starts_with("\xff\xfe") || defaultText.starts_with("\xfe\xff")) {
            error = "Cannot read default INI or unsupported encoding"; return false;
        }
        std::error_code ec;
        const bool exists = std::filesystem::exists(userPath, ec);
        if (ec) { error = "Cannot inspect user INI: " + ec.message(); return false; }
        if (!exists) {
            std::istringstream input(defaultText);
            return ParseRuntimeTuningIni(input, current, result, error, loggingOnly);
        }
        std::ifstream user(userPath, std::ios::binary);
        if (!user) { error = "Cannot open user INI: " + userPath.string(); return false; }
        // User files contain only menu overrides; hidden/default keys remain owned by defaults.
        std::string filtered, section, line;
        while (std::getline(user, line)) {
            if (line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
            if (line.find('\0') != std::string::npos || line.starts_with("\xff\xfe") || line.starts_with("\xfe\xff")) {
                error = "Unsupported user INI encoding"; return false;
            }
            const auto comment = line.find_first_of("#;");
            auto active = line.substr(0, comment);
            const auto trim = [](std::string value) {
                auto first = value.find_first_not_of(" \t\r");
                if (first == std::string::npos) return std::string{};
                return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
            };
            active = trim(active);
            if (active.empty()) continue;
            if (active.front() == '[') {
                section = active.back() == ']' ? trim(active.substr(1, active.size()-2)) : "";
                filtered += "[" + section + "]\n";
                continue;
            }
            const auto equals = active.find('=');
            if (equals == std::string::npos) continue;
            const auto key = trim(active.substr(0, equals));
            for (const auto& setting : GetMenuSettingsCatalog()) {
                if (section == setting.section && key == setting.key) {
                    filtered += active + "\n";
                    break;
                }
            }
        }
        if (user.bad()) { error = "Cannot read user INI"; return false; }
        // The empty section prevents headerless user text inheriting the defaults' last section.
        std::istringstream overrides(defaultText + "\n[]\n" + filtered);
        return ParseRuntimeTuningIni(overrides, current, result, error, loggingOnly);
    }
}
