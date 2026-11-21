#pragma once
#include <string>

namespace Mus {
    enum class TextureCompressionBackend { None, CPU, GPU };
    struct TextureCompressionPlan {
        TextureCompressionBackend backend;
        int quality;
    };
    inline TextureCompressionPlan SelectTextureCompression(int requested, int quality, bool secondGPU, bool immediate) {
        return {requested == 0 ? TextureCompressionBackend::None :
            (requested == 2 || (requested == -1 && (secondGPU || immediate))) ?
                TextureCompressionBackend::GPU : TextureCompressionBackend::CPU, quality};
    }
    inline std::string TextureCompressionSignature(const TextureCompressionPlan& plan) {
        const char* backend = plan.backend == TextureCompressionBackend::None ? "none" :
            plan.backend == TextureCompressionBackend::GPU ? "gpu-bc7" : "cpu-bc7";
        return std::string("CompressionBackend=") + backend + ";CompressionQuality=" + std::to_string(plan.quality) + ";";
    }
}
