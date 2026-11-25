"""Generate an offline C++ differential test of the actual LoadCacheResource bodies.
Usage: python tests/load_cache_equivalence.py OUTPUT.cpp [BASE_REVISION]
Then compile OUTPUT.cpp with a C++20 compiler and run it. Game/D3D services are
stubbed; both implementations share identical deterministic cache responses.
"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
baseline = sys.argv[2] if len(sys.argv) > 2 else "ee4034cdfe0aa8d102cddfc10f23859635f7c7a6"
old = subprocess.check_output(["git", "show", f"{baseline}:src/ObjectNormalMapUpdater.cpp"], cwd=root).decode("utf-8-sig")
new = (root / "src/ObjectNormalMapUpdater.cpp").read_text(encoding="utf-8-sig")

def body(source, name):
    start = source.rindex("\n", 0, source.index("ObjectNormalMapUpdater::LoadCacheResource(")) + 1
    end = source.index("bool ObjectNormalMapUpdater::CreateGeometryResourceData", start)
    extracted = source[start:end].replace("ObjectNormalMapUpdater::GeometryIndex", "GeometryIndex", 1)
    extracted = extracted.replace("ObjectNormalMapUpdater::LoadCacheResource", name, 1)
    # Signature construction is tested separately. Preserve the same deterministic
    # hash service for old two-argument and new batch-signature call sites.
    return extracted.replace("const std::string& generationSignature)",
                             "const std::string& generationSignature = std::string{})", 1)

prefix = r"""
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
namespace RE { using FormID = std::uint32_t; struct BSGeometry {}; }
namespace logger {
    template<class... Args> void error(Args&&...) {}
    template<class... Args> void info(Args&&...) {}
}
struct GeometryData {
    struct GeometriesInfo {
        RE::BSGeometry* geometry;
        struct Info { unsigned count; unsigned vertexCount() const { return count; } } objInfo;
        std::uint64_t hash;
    };
    std::vector<GeometriesInfo> geometries;
};
using GeometryDataPtr = std::shared_ptr<GeometryData>;
using GeometryIndex = std::unordered_map<RE::BSGeometry*, GeometryData::GeometriesInfo*>;
struct UpdateTextureSet {
    unsigned slot;
    std::string geometryName, textureName, srcTexturePath, detailTexturePath;
    bool operator==(const UpdateTextureSet&) const = default;
};
using UpdateSet = std::vector<std::pair<RE::BSGeometry*, UpdateTextureSet>>;
struct TextureResource { std::vector<std::uint64_t> componentHashes; };
using TextureResourcePtr = std::shared_ptr<TextureResource>;
struct NormalMapResult {
    unsigned slot = 0, vertexCount = 0;
    RE::BSGeometry* geometry = nullptr;
    std::string geoName, texturePath, textureName;
    TextureResourcePtr texture;
    std::uint64_t hash = 0;
    bool existResource = false, diskCache = false;
    bool operator==(const NormalMapResult&) const = default;
};
using UpdateResult = std::vector<NormalMapResult>;
struct TextureResourceData { std::string textureName; };
using TextureResourceDataPtr = std::shared_ptr<TextureResourceData>;
using ResourceDatas = std::vector<TextureResourceDataPtr>;
using MergedTextureGeometries = std::unordered_set<RE::BSGeometry*>;
struct NormalMapStore {
    std::unordered_map<std::uint64_t, TextureResourcePtr> cache;
    std::vector<std::uint64_t> calls;
    static NormalMapStore& GetSingleton() { static NormalMapStore store; return store; }
    void GetResource(std::uint64_t hash, TextureResourcePtr& resource, bool& disk) {
        calls.push_back(hash);
        const auto found = cache.find(hash);
        resource = found == cache.end() ? nullptr : found->second;
        disk = (hash & 1) != 0;
    }
};
std::uint64_t GetHash(const UpdateTextureSet& update, std::uint64_t geometryHash, const std::string& = {}) {
    auto value = geometryHash * 1099511628211ull + update.slot;
    for (unsigned char c : update.textureName) value = value * 131 + c;
    return value;
}
"""
suffix = r"""
void Check(bool value, const char* what, unsigned seed) {
    if (!value) throw std::runtime_error(std::string(what) + " case=" + std::to_string(seed));
}
int main() {
    std::array<RE::BSGeometry, 24> objects;
    std::mt19937 random(0x20261121);
    auto& store = NormalMapStore::GetSingleton();
    unsigned populatedResults = 0, totalCalls = 0;
    for (unsigned seed = 0; seed < 12000; ++seed) {
        const auto identity = [&](unsigned n) -> RE::BSGeometry* { return n == 23 ? nullptr : &objects[n]; };
        auto original = std::make_shared<GeometryData>();
        const unsigned geometryCount = random() % 32;
        for (unsigned i = 0; i < geometryCount; ++i)
            original->geometries.push_back({identity(random() % 24), {unsigned(random() % 5)}, random() % 32});
        UpdateSet updates;
        for (unsigned i = 0, n = random() % 42; i < n; ++i)
            updates.push_back({identity(random() % 24), {unsigned(random() % 3), std::to_string(i),
                "group" + std::to_string(random() % 6), (i % 2) ? "source" : "", "detail"}});
        // Seed both complete and mismatched resources. Repeated update identities
        // intentionally combine their hashes repeatedly, as the production API does.
        auto seeded = *original;
        for (const auto& update : updates) {
            auto found = std::find_if(seeded.geometries.begin(), seeded.geometries.end(),
                [&](const auto& geometry) { return geometry.geometry == update.first; });
            if (found != seeded.geometries.end()) found->hash = GetHash(update.second, found->hash);
        }
        std::map<std::string, std::vector<std::uint64_t>> groups;
        for (const auto& update : updates) {
            auto found = std::find_if(seeded.geometries.begin(), seeded.geometries.end(),
                [&](const auto& geometry) { return geometry.geometry == update.first; });
            if (found != seeded.geometries.end()) groups[update.second.textureName].push_back(found->hash);
        }
        store.cache.clear();
        for (auto& [name, hashes] : groups) {
            std::sort(hashes.begin(), hashes.end());
            hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
            for (auto hash : hashes) {
                if (random() % 3 == 0) continue;
                auto resource = std::make_shared<TextureResource>();
                resource->componentHashes = hashes;
                if (random() % 4 == 0) resource->componentHashes.push_back(12345);
                store.cache[hash] = resource;
            }
        }
        UpdateResult initial;
        // Existing results exercise the sharing pass and its first eligible match.
        for (unsigned i = 0, n = random() % 4; i < n; ++i) {
            NormalMapResult result;
            result.geometry = identity(random() % 24);
            result.textureName = "group" + std::to_string(random() % 6);
            result.vertexCount = random() % 5;
            result.existResource = random() % 2;
            result.diskCache = random() % 2;
            result.texture = std::make_shared<TextureResource>();
            initial.push_back(result);
        }
        auto a = std::make_shared<GeometryData>(*original), b = std::make_shared<GeometryData>(*original);
        auto ua = updates, ub = updates;
        auto ra = initial, rb = initial;
        ResourceDatas da, db;
        MergedTextureGeometries ma, mb;
        store.calls.clear();
        Baseline(7, a, ua, ma, da, ra);
        const auto calls = store.calls;
        store.calls.clear();
        const auto geometryIndex = Optimized(7, b, ub, mb, db, rb);
        for (const auto& update : ub) {
            const auto first = std::find_if(b->geometries.begin(), b->geometries.end(),
                [&](const auto& geometry) { return geometry.geometry == update.first; });
            Check(first != b->geometries.end() && geometryIndex.at(update.first) == &*first,
                "remaining generation uses first geometry identity", seed);
        }
        Check(ua == ub, "remaining updates/order", seed);
        Check(ra == rb, "cache results/order/aliases", seed);
        Check(ma == mb, "merged geometries", seed);
        Check(calls == store.calls, "cache lookup sequence", seed);
        Check(da.size() == db.size(), "resource-data count", seed);
        for (std::size_t i = 0; i < da.size(); ++i)
            Check(da[i]->textureName == db[i]->textureName, "resource-data order", seed);
        for (std::size_t i = 0; i < a->geometries.size(); ++i)
            Check(a->geometries[i].hash == b->geometries[i].hash, "hash/first-match mutation", seed);
        populatedResults += ra.size() > initial.size();
        totalCalls += calls.size();
    }
    std::cout << "PASS: 12000 complete-body differential cases; result-producing cases="
              << populatedResults << "; cache lookups=" << totalCalls << '\n';
}
"""
Path(sys.argv[1]).write_text(prefix + body(old, "Baseline") + body(new, "Optimized") + suffix, encoding="utf-8")
print(f"Generated differential harness against {baseline}")
