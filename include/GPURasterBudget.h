#pragma once
#include "CPURasterTiles.h"

namespace Mus {
    // Match HLSL's inclusive floor/ceil box in floating-point pixel space.
    // A CPU tile's truncated integer projection would undercount fractional UVs.
    inline bool ReserveGPURasterSamples(const DirectX::XMFLOAT2& a, const DirectX::XMFLOAT2& b,
        const DirectX::XMFLOAT2& c, unsigned width, unsigned height, std::uint64_t& geometryRemaining,
        std::uint64_t& batchRemaining, bool& largeTriangle) {
        const float x[]{a.x * width, b.x * width, c.x * width};
        const float y[]{a.y * height, b.y * height, c.y * height};
        for (unsigned i = 0; i < 3; ++i)
            if (!(std::abs(x[i]) < 0x1p24f && std::abs(y[i]) < 0x1p24f)) return false;
        // Match the shader's precise determinant, including separate rounding
        // of the products. Degenerate triangles never enter its pixel loops.
        const volatile float product0 = (x[2] - x[0]) * (y[1] - y[0]);
        const volatile float product1 = (y[2] - y[0]) * (x[1] - x[0]);
        if (std::abs(product0 - product1) < 1e-6f) return true;
        const auto span = [](const float (&v)[3]) {
            const auto low = static_cast<int>(std::floor(std::min({v[0], v[1], v[2]})));
            const auto high = static_cast<int>(std::ceil(std::max({v[0], v[1], v[2]})));
            return static_cast<std::uint64_t>(high - low + 1);
        };
        const auto spanX = span(x), spanY = span(y);
        const auto samples = spanX * spanY;
        if (samples > geometryRemaining || samples > batchRemaining) return false;
        if (samples > 32768) {
            // Match FirstLaneCoordinate: each complete wrapped period contributes
            // at most ceil(extent/8) coordinates to a lane. The tail may straddle
            // one NPOT seam and therefore contribute one additional coordinate.
            const auto maxLaneSpan = [](std::uint64_t length, unsigned extent) {
                if (extent < 8) return length;
                const auto tail = length % extent;
                return length / extent * ((extent + 7) / 8) +
                    (tail ? (tail + 7) / 8 + 1 : 0);
            };
            constexpr std::uint64_t maxSamplesPerLane = 4ull * 1024 * 1024;
            if (maxLaneSpan(spanX, width) * maxLaneSpan(spanY, height) > maxSamplesPerLane)
                return false;
        }
        geometryRemaining -= samples;
        batchRemaining -= samples;
        largeTriangle |= samples > 32768;
        return true;
    }
}
