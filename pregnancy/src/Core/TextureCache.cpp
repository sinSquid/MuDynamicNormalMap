#include "pch.h"
#include "Core/TextureCache.h"
#include "Core/Config.h"

namespace PNS {

    void TextureCache::ScanAll() {
        std::unique_lock lock(mutex_);

        textures_.clear();
        raceRedirects_.clear();
        raceInfo_.clear();

        // Base path: Data/textures/PNS/
        auto dataDir = fs::path("Data") / Config::GetSingleton().GetBaseDirectory();

        if (!fs::exists(dataDir)) {
            logger::error("TextureCache: Base directory not found: {}", dataDir.string());
            return;
        }

        logger::debug("TextureCache: Scanning {}", dataDir.string());

        for (const auto& entry : fs::directory_iterator(dataDir)) {
            if (!entry.is_directory()) continue;
            ScanRaceDirectory(dataDir, entry.path().filename().string());
        }

        logger::debug("TextureCache: Scan complete. {} textures cached across {} race dirs",
                     textures_.size(), raceInfo_.size());

        // Dump all cached texture keys for debugging
        for (const auto& [key, path] : textures_) {
            logger::debug("  cached: race='{}' female={} w={} p={} sbm={} -> '{}'",
                         key.raceDir, key.isFemale, key.weightIndex, key.partition, key.sbmLevel, path);
        }
        for (const auto& [name, info] : raceInfo_) {
            logger::debug("  raceInfo: '{}' maxWeight={} maxPartition={} maxSBM={} female={} male={}",
                         name, info.maxWeightIndex, info.maxPartition, info.maxSBMLevel,
                         info.hasFemale, info.hasMale);
        }
        for (const auto& [from, to] : raceRedirects_) {
            logger::debug("  redirect: '{}' -> '{}'", from, to);
        }
    }

    void TextureCache::ScanRaceDirectory(const fs::path& baseDir, const std::string& dirName) {
        auto dirPath = baseDir / dirName;

        // Check for Use.txt redirect
        auto useTxt = dirPath / "Use.txt";

        if (fs::exists(useTxt)) {
            std::ifstream file(useTxt);
            std::string redirect;
            if (std::getline(file, redirect)) {
                // Trim whitespace
                redirect.erase(0, redirect.find_first_not_of(" \t\r\n"));
                redirect.erase(redirect.find_last_not_of(" \t\r\n") + 1);
                if (!redirect.empty()) {
                    // CI map handles case — store original names
                    raceRedirects_[dirName] = redirect;
                    logger::debug("TextureCache: {} -> {} (Use.txt redirect)", dirName, redirect);
                    return;
                }
            }
        }

        // Scan female/ and male/ subdirectories
        auto femaleDir = dirPath / "female";
        auto maleDir = dirPath / "male";

        if (fs::exists(femaleDir) && fs::is_directory(femaleDir)) {
            ParseTextureFiles(femaleDir, dirName, true);
        }
        if (fs::exists(maleDir) && fs::is_directory(maleDir)) {
            ParseTextureFiles(maleDir, dirName, false);
        }
    }

    void TextureCache::ParseTextureFiles(const fs::path& genderDir, const std::string& raceDir,
                                          bool isFemale) {
        // Pattern: (female|male)body_(\d+)_msn_p(\d+)(?:_sbm(\d+))?\.dds
        static const std::regex pattern(
            R"((female|male)body_(\d+)_msn_p(\d+)(?:_sbm(\d+))?\.dds)",
            std::regex_constants::icase
        );

        auto& info = raceInfo_[raceDir];
        info.resolvedDir = raceDir;
        if (isFemale) info.hasFemale = true;
        else info.hasMale = true;

        for (const auto& entry : fs::directory_iterator(genderDir)) {
            if (!entry.is_regular_file()) continue;

            auto filename = entry.path().filename().string();
            std::smatch match;

            if (!std::regex_match(filename, match, pattern)) continue;

            int weightIndex = std::stoi(match[2].str());
            int partition = std::stoi(match[3].str());
            int sbmLevel = match[4].matched ? std::stoi(match[4].str()) : 0;

            info.maxWeightIndex = std::max(info.maxWeightIndex, weightIndex);
            info.maxPartition = std::max(info.maxPartition, partition);
            info.maxSBMLevel = std::max(info.maxSBMLevel, sbmLevel);

            // Store texture path relative to Data/ using backslashes (Skyrim convention)
            auto genderStr = isFemale ? "female" : "male";
            auto texPath = fmt::format("{}\\{}\\{}\\{}",
                Config::GetSingleton().GetBaseDirectory(), raceDir, genderStr, filename);
            // Normalize any forward slashes from config base directory
            std::replace(texPath.begin(), texPath.end(), '/', '\\');

            TextureKey key{raceDir, isFemale, weightIndex, partition, sbmLevel};
            textures_[key] = texPath;
        }

        logger::debug("TextureCache: {} {}: weights=1..{}, partitions=1..{}, sbm=0..{}",
                     raceDir, isFemale ? "female" : "male",
                     info.maxWeightIndex, info.maxPartition, info.maxSBMLevel);
    }

    std::string TextureCache::Get(const std::string& raceDir, bool isFemale, float actorWeight,
                                   int partition, int sbmLevel) const {
        std::shared_lock lock(mutex_);

        // CI maps handle case automatically — no manual lowering needed
        std::string resolved = raceDir;
        if (auto it = raceRedirects_.find(raceDir); it != raceRedirects_.end()) {
            resolved = it->second;
            logger::debug("TextureCache::Get: redirect '{}' -> '{}'", raceDir, resolved);
        }

        // Get race info to determine weight count
        auto infoIt = raceInfo_.find(resolved);
        if (infoIt == raceInfo_.end()) {
            logger::debug("TextureCache::Get: no raceInfo for '{}', falling back to _default", resolved);
            resolved = "_default";
            infoIt = raceInfo_.find(resolved);
            if (infoIt == raceInfo_.end()) {
                logger::debug("TextureCache::Get: no _default raceInfo either, returning empty");
                return "";
            }
        }

        int weightIndex = ComputeWeightIndex(actorWeight, infoIt->second.maxWeightIndex);
        logger::debug("TextureCache::Get: resolved='{}' female={} weight={:.1f} -> weightIndex={} (max={}) p={} sbm={}",
                     resolved, isFemale, actorWeight, weightIndex, infoIt->second.maxWeightIndex, partition, sbmLevel);

        TextureKey key{resolved, isFemale, weightIndex, partition, sbmLevel};
        if (auto it = textures_.find(key); it != textures_.end()) {
            logger::debug("TextureCache::Get: FOUND '{}'", it->second);
            return it->second;
        }

        // If SBM level not found, try without it
        if (sbmLevel > 0) {
            key.sbmLevel = 0;
            if (auto it = textures_.find(key); it != textures_.end()) {
                logger::debug("TextureCache::Get: FOUND (sbm fallback) '{}'", it->second);
                return it->second;
            }
        }

        logger::debug("TextureCache::Get: NOT FOUND for race='{}' female={} w={} p={} sbm={}",
                     resolved, isFemale, weightIndex, partition, sbmLevel);
        return "";
    }

    std::string TextureCache::ResolveRaceDir(const std::string& raceEditorID) const {
        std::shared_lock lock(mutex_);

        // CI maps match regardless of case
        if (auto it = raceInfo_.find(raceEditorID); it != raceInfo_.end()) {
            return it->first;  // return the key as stored (original casing from disk)
        }

        if (auto it = raceRedirects_.find(raceEditorID); it != raceRedirects_.end()) {
            return it->second;
        }

        return "_default";
    }

    const RaceTextureInfo* TextureCache::GetRaceInfo(const std::string& raceDir) const {
        std::shared_lock lock(mutex_);

        std::string resolved = raceDir;
        if (auto it = raceRedirects_.find(raceDir); it != raceRedirects_.end()) {
            resolved = it->second;
        }

        if (auto it = raceInfo_.find(resolved); it != raceInfo_.end()) {
            return &it->second;
        }

        if (auto it = raceInfo_.find("_default"); it != raceInfo_.end()) {
            return &it->second;
        }

        return nullptr;
    }

    int TextureCache::ComputeWeightIndex(float actorWeight, int maxWeightIndex) {
        if (maxWeightIndex <= 0) return 1;
        int index = static_cast<int>(actorWeight / (100.0f / maxWeightIndex) + 1);
        return std::clamp(index, 1, maxWeightIndex);
    }

    std::string TextureCache::GetPregnancyTarget(const std::string& raceDir, bool isFemale, float actorWeight) const {
        std::shared_lock lock(mutex_);
        auto lookup = [&](const std::string& directory) {
            int maxWeight = 0;
            for (const auto& [key,path] : textures_) if (CaseInsensitiveEqual{}(key.raceDir,directory) && key.isFemale == isFemale && key.sbmLevel == 0)
                maxWeight = std::max(maxWeight,key.weightIndex);
            const int wanted = ComputeWeightIndex(actorWeight,maxWeight);
            const TextureKey* best = nullptr; const std::string* result = nullptr;
            for (const auto& [key,path] : textures_) {
                if (!CaseInsensitiveEqual{}(key.raceDir,directory) || key.isFemale != isFemale || key.sbmLevel != 0 || key.partition <= 0) continue;
                if (!best || std::abs(key.weightIndex-wanted) < std::abs(best->weightIndex-wanted) ||
                    (std::abs(key.weightIndex-wanted) == std::abs(best->weightIndex-wanted) &&
                        (key.weightIndex < best->weightIndex || (key.weightIndex == best->weightIndex && key.partition > best->partition)))) {
                    best = &key; result = &path;
                }
            }
            return result ? *result : std::string{};
        };
        auto result = lookup(raceDir);
        if (result.empty()) result = lookup("_Default");
        return result;
    }

}  // namespace PNS


