#pragma once
#include "RuntimeTuning.h"
#include <filesystem>
#include <string>

namespace Mus::IniConfig {
    // Saves the menu catalog to the user override; hidden INI settings are preserved.
    // Keeps the previous bytes in .menu.bak and commits a closed temporary file by rename.
    // Does not apply settings. Failures include the stage, path and Win32 error when applicable.
    bool SaveRuntimeTuning(const std::filesystem::path& path, const RuntimeTuning& tuning, std::string& error);
}
