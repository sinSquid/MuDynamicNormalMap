#pragma once
#include <array>

namespace PNS {

    // Case-insensitive string hash and equality for unordered_map keys.
    // All race/directory lookups are automatically case-insensitive.
    struct CaseInsensitiveHash {
        std::size_t operator()(const std::string& s) const {
            std::size_t h = 0;
            for (unsigned char c : s) {
                h = h * 31 + std::tolower(c);
            }
            return h;
        }
    };

    struct CaseInsensitiveEqual {
        bool operator()(const std::string& a, const std::string& b) const {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }
            return true;
        }
    };

    template <typename V>
    using CIStringMap = std::unordered_map<std::string, V, CaseInsensitiveHash, CaseInsensitiveEqual>;

    struct TextureKey {
        std::string raceDir;     // e.g. "_default", "ArgonianRace"
        bool isFemale;
        int weightIndex;         // 1-based
        int partition;           // 1-based
        int sbmLevel;            // 0 = no SBM
    };

    struct TextureKeyHash {
        std::size_t operator()(const TextureKey& k) const {
            auto h = CaseInsensitiveHash{}(k.raceDir);
            h ^= std::hash<bool>{}(k.isFemale) << 1;
            h ^= std::hash<int>{}(k.weightIndex) << 2;
            h ^= std::hash<int>{}(k.partition) << 3;
            h ^= std::hash<int>{}(k.sbmLevel) << 4;
            return h;
        }
    };

    struct TextureKeyEqual {
        bool operator()(const TextureKey& a, const TextureKey& b) const {
            return CaseInsensitiveEqual{}(a.raceDir, b.raceDir)
                && a.isFemale == b.isFemale
                && a.weightIndex == b.weightIndex
                && a.partition == b.partition
                && a.sbmLevel == b.sbmLevel;
        }
    };

    struct RaceTextureInfo {
        std::string resolvedDir;   // after Use.txt redirect
        int maxWeightIndex = 0;
        int maxPartition = 0;
        int maxSBMLevel = 0;
        // Unsuffixed male/female packs can have different bins/stages; SBM
        // variants must not inflate the ordinary static-mode selection range.
        std::array<int, 2> weightCounts{}; // male, female
        std::array<int, 2> partitionCounts{};
        bool hasFemale = false;
        bool hasMale = false;
    };

    class TextureCache {
    public:
        static TextureCache& GetSingleton() {
            static TextureCache instance;
            return instance;
        }

        // Scan all textures/PNS/ directories at startup
        void ScanAll();

        // Look up a texture path for given parameters. Returns empty string if not found.
        std::string Get(const std::string& raceDir, bool isFemale, float actorWeight,
                        int partition, int sbmLevel = 0) const;

        // Resolve race directory (handles Use.txt redirects)
        std::string ResolveRaceDir(const std::string& raceEditorID) const;

        // Get info about what's available for a race
        const RaceTextureInfo* GetRaceInfo(const std::string& raceDir) const;

        // Compute weight index from actor weight (0-100) and max weight count
        static int ComputeWeightIndex(float actorWeight, int maxWeightIndex);

        // Select a real available highest stage for this gender and nearest weight.
        // Dynamic mode treats it as an optional endpoint, never as a stepped progression.
        std::string GetPregnancyTarget(const std::string& raceDir, bool isFemale, float actorWeight) const;

    private:
        TextureCache() = default;

        void ScanRaceDirectory(const fs::path& baseDir, const std::string& dirName);
        void ParseTextureFiles(const fs::path& genderDir, const std::string& raceDir,
                               bool isFemale);

        // race editor ID -> resolved directory name (after Use.txt)
        CIStringMap<std::string> raceRedirects_;

        // resolved directory name -> texture info
        CIStringMap<RaceTextureInfo> raceInfo_;

        // Full lookup table: key -> texture path (relative to Data/)
        std::unordered_map<TextureKey, std::string, TextureKeyHash, TextureKeyEqual> textures_;

        mutable std::shared_mutex mutex_;
    };

}  // namespace PNS
