#pragma once
#include "RuntimeTuning.h"
#include <filesystem>
#include <string>

namespace Mus::IniConfig {
    // Saves the menu catalog; hidden INI settings are preserved. Does not apply settings.
    bool SaveRuntimeTuning(const std::filesystem::path& path, const RuntimeTuning& tuning, std::string& error);
}
