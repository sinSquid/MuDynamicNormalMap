"""Generate a standalone test of the production texture cache key function."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
source = (root / "src/ObjectNormalMapUpdater.cpp").read_text(encoding="utf-8-sig")
start = source.index("std::uint64_t ObjectNormalMapUpdater::GetHash(")
end = source.index("\n\tObjectNormalMapUpdater::UpdateResult", start)
function = source[start:end].replace("ObjectNormalMapUpdater::GetHash", "GetHash")
prefix = r"""
#include <algorithm>
#include <cassert>
#include <cmath>
#include <functional>
#include <sstream>
#include "SettingsCatalog.h"
using namespace Mus;
struct UpdateTextureSet {
    unsigned slot = 32;
    std::string srcTexturePath = "textures/base.dds";
    std::string detailTexturePath = "textures/detail.dds";
    std::string overlayTexturePath;
    std::string maskTexturePath;
    float detailStrength = 0.5f;
};
"""
main = r"""
int main() {
    RuntimeTuning settings;
    settings.TextureCompress = -1;
    const auto cpu = SelectTextureCompression(-1, 1, false, false);
    const auto gpu = SelectTextureCompression(-1, 1, true, false);
    const auto immediate = SelectTextureCompression(-1, 1, false, true);
    assert(cpu.backend == TextureCompressionBackend::CPU);
    assert(gpu.backend == TextureCompressionBackend::GPU);
    assert(immediate.backend == gpu.backend);
    const auto cpuKey = RuntimeGenerationSignature(settings, true, cpu);
    const auto gpuKey = RuntimeGenerationSignature(settings, true, gpu);
    const auto cpuRenderKey = RuntimeGenerationSignature(settings, false, gpu);
    UpdateTextureSet original;
    auto adjacent = original;
    adjacent.detailStrength = std::nextafter(original.detailStrength, 1.0f);
    assert(GetHash(original, 42, cpuKey) != GetHash(adjacent, 42, cpuKey));
    assert(GetHash(original, 42, cpuKey) != GetHash(original, 42, gpuKey));
    assert(GetHash(original, 42, gpuKey) != GetHash(original, 42, cpuRenderKey));
    assert(GetHash(original, 42, gpuKey) ==
        GetHash(original, 42, RuntimeGenerationSignature(settings, true, immediate)));
}
"""
Path(sys.argv[1]).write_text(prefix + function + main, encoding="utf-8")
