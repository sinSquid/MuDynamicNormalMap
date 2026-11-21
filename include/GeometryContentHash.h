#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <xxhash.h>
namespace Mus {
    inline std::uint64_t GeometryContentHash(std::span<const std::byte> normals,
        std::span<const std::byte> uvs, std::span<const std::byte> tangents,
        std::span<const std::byte> bitangents, std::span<const std::uint32_t> indices,
        std::uint32_t vertexStart) {
        std::vector<std::uint32_t> localIndices;
        localIndices.reserve(indices.size());
        for (auto index : indices) localIndices.push_back(index - vertexStart);
        // Domain marker also prevents reusing old normal-only cache keys.
        const std::array<std::uint64_t, 6> parts{0x4d444e4d47454f34ull,
            XXH3_64bits(normals.data(), normals.size()), XXH3_64bits(uvs.data(), uvs.size()),
            XXH3_64bits(tangents.data(), tangents.size()), XXH3_64bits(bitangents.data(), bitangents.size()),
            XXH3_64bits(localIndices.data(), localIndices.size() * sizeof(std::uint32_t))};
        return XXH3_64bits(parts.data(), sizeof(parts));
    }
}
