#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
namespace Mus {
    struct TextureRowSlice { std::uint32_t top, bottom; };
    // BC regions must start on whole blocks; only the final edge may be partial.
    inline std::vector<TextureRowSlice> MakeTextureRowSlices(std::uint32_t height,
        std::uint32_t requestedSlices, std::uint32_t blockHeight) {
        if (!height || !requestedSlices || !blockHeight) return {};
        const auto rows = (height - 1) / blockHeight + 1;
        const auto count = std::min(rows, requestedSlices);
        const auto rowsPerSlice = (rows - 1) / count + 1;
        std::vector<TextureRowSlice> slices;
        slices.reserve(count);
        for (std::uint32_t row = 0; row < rows; row += rowsPerSlice)
            slices.push_back({row * blockHeight, std::min(height, (row + rowsPerSlice) * blockHeight)});
        return slices;
    }
}
