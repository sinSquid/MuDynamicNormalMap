#pragma once
#include "RuntimeTuning.h"
#include <filesystem>
#include <string>

namespace Mus::IniConfig {
    // Saves only the four runtime-tuning keys. Does not apply runtime settings.
    bool SaveRuntimeTuning(const std::filesystem::path& path, const RuntimeTuning& tuning, std::string& error);
}
