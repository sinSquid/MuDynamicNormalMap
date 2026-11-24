#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <DirectXMath.h>

namespace Mus {
    inline constexpr int RasterTileSize = 32;
    struct RasterTriangle {
        DirectX::XMINT2 p0, p1, p2;
        int left, top, right, bottom;
        std::uint32_t index;
        DirectX::XMFLOAT2 edge0, edge1;
        float d00, d01, d11, denominator;

        bool Covers(float x, float y, DirectX::XMFLOAT3& bary) const {
            using namespace DirectX;
            const auto offset = XMVectorSet(x - p0.x, y - p0.y, 0, 0);
            const float d20 = XMVectorGetX(XMVector2Dot(offset, XMLoadFloat2(&edge0)));
            const float d21 = XMVectorGetX(XMVector2Dot(offset, XMLoadFloat2(&edge1)));
            const float v = (d11 * d20 - d01 * d21) / denominator;
            const float w = (d00 * d21 - d01 * d20) / denominator;
            const float u = 1.0f - v - w;
            if (u < 0 || v < 0 || w < 0) return false;
            bary = {u, v, w};
            return true;
        }
    };
    struct RasterRegion {
        std::uint32_t triangle;
        int left, top, right, bottom;
    };

    // Charge bounding-box samples and the wide-triangle row lookup overhead.
    // The tile split partitions the box, even across multiple UV periods.
    inline constexpr std::uint64_t RasterBatchSampleLimit = 512ull * 1024 * 1024;

    inline bool UsesWideRasterBins(const RasterTriangle& triangle) {
        const auto columns = (triangle.right - triangle.left) / RasterTileSize + 3;
        const auto rows = (triangle.bottom - triangle.top) / RasterTileSize + 3;
        return static_cast<std::uint64_t>(columns) * rows > 256;
    }

    inline std::uint64_t RasterGeometrySampleLimit(std::uint32_t width, std::uint32_t height) {
        return std::clamp<std::uint64_t>(16ull * width * height, 16ull * 1024 * 1024, RasterBatchSampleLimit);
    }

    inline bool ReserveRasterSamples(const RasterTriangle& triangle, int width, int height,
        std::uint64_t& geometryRemaining, std::uint64_t& batchRemaining) {
        auto count = static_cast<std::uint64_t>(triangle.right - triangle.left) *
            static_cast<std::uint64_t>(triangle.bottom - triangle.top);
        if (UsesWideRasterBins(triangle)) {
            const auto rows = std::min((height + RasterTileSize - 1) / RasterTileSize,
                (triangle.bottom - triangle.top) / RasterTileSize + 3);
            const auto columns = (width + RasterTileSize - 1) / RasterTileSize;
            count += static_cast<std::uint64_t>(rows) * columns;
        }
        if (count > geometryRemaining || count > batchRemaining) return false;
        geometryRemaining -= count;
        batchRemaining -= count;
        return true;
    }

    inline bool ProjectRasterTriangle(RasterTriangle& result, const DirectX::XMFLOAT2& a,
        const DirectX::XMFLOAT2& b, const DirectX::XMFLOAT2& c, int width, int height, std::uint32_t index)
    {
        const auto project = [&](const DirectX::XMFLOAT2& uv, DirectX::XMINT2& pixel) {
            const float x = uv.x * width, y = uv.y * height;
            // Exclude coordinates the integer rasterizer cannot safely represent.
            if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) >= 0x1p24f || std::abs(y) >= 0x1p24f)
                return false;
            pixel = {static_cast<int>(x), static_cast<int>(y)};
            return true;
        };
        if (!project(a, result.p0) || !project(b, result.p1) || !project(c, result.p2)) return false;
        result.left = std::min({result.p0.x, result.p1.x, result.p2.x});
        result.top = std::min({result.p0.y, result.p1.y, result.p2.y});
        result.right = std::max({result.p0.x, result.p1.x, result.p2.x}) + 1;
        result.bottom = std::max({result.p0.y, result.p1.y, result.p2.y}) + 1;
        result.index = index;
        using namespace DirectX;
        result.edge0 = {static_cast<float>(result.p1.x - result.p0.x), static_cast<float>(result.p1.y - result.p0.y)};
        result.edge1 = {static_cast<float>(result.p2.x - result.p0.x), static_cast<float>(result.p2.y - result.p0.y)};
        const auto edge0 = XMLoadFloat2(&result.edge0), edge1 = XMLoadFloat2(&result.edge1);
        result.d00 = XMVectorGetX(XMVector2Dot(edge0, edge0));
        result.d01 = XMVectorGetX(XMVector2Dot(edge0, edge1));
        result.d11 = XMVectorGetX(XMVector2Dot(edge1, edge1));
        result.denominator = result.d00 * result.d11 - result.d01 * result.d01;
        return result.denominator != 0.0f;
    }

    inline int RasterFloorDiv(int value, int extent) {
        return value / extent - (value % extent < 0 ? 1 : 0);
    }

    class CPURasterTiles {
        struct TileFootprint {
            int first, last, wrappedLast;
            bool Contains(int tile) const {
                return (tile >= first && tile <= last) || tile <= wrappedLast;
            }
        };
        struct WideTriangle {
            std::uint32_t id;
            TileFootprint columns;
        };

    public:
        CPURasterTiles(int width, int height) :
            width(width), height(height), columns((width + RasterTileSize - 1) / RasterTileSize),
            wideRows((height + RasterTileSize - 1) / RasterTileSize),
            bins(static_cast<std::size_t>(columns) * wideRows.size()) {}

        void Add(RasterTriangle triangle) {
            const auto id = static_cast<std::uint32_t>(triangles.size());
            triangles.push_back(triangle);
            // Very large triangles are clipped on demand to avoid replicating
            // them into thousands of buckets.
            if (UsesWideRasterBins(triangle)) {
                const auto rowFootprint = Footprint(triangle.top, triangle.bottom, height);
                const auto columnFootprint = Footprint(triangle.left, triangle.right, width);
                for (int row = 0; row < static_cast<int>(wideRows.size()); ++row)
                    if (rowFootprint.Contains(row)) wideRows[row].push_back({id, columnFootprint});
                return;
            }
            WalkAxis(triangle.top, triangle.bottom, height, [&](int row, int top, int bottom) {
                WalkAxis(triangle.left, triangle.right, width, [&](int col, int left, int right) {
                    bins[row * columns + col].push_back({id, left, top, right, bottom});
                });
            });
        }

        std::size_t Size() const { return bins.size(); }
        bool HasRegions(std::size_t tile) const {
            if (!bins[tile].empty()) return true;
            const int column = static_cast<int>(tile % columns);
            const auto& row = wideRows[tile / columns];
            return std::any_of(row.begin(), row.end(), [column](const auto& entry) {
                return entry.columns.Contains(column);
            });
        }
        int TileX(std::size_t tile) const { return static_cast<int>(tile % columns) * RasterTileSize; }
        int TileY(std::size_t tile) const { return static_cast<int>(tile / columns) * RasterTileSize; }

        template <class Visit>
        void VisitRegions(std::size_t tile, Visit&& visit) const {
            for (const auto& region : bins[tile]) visit(triangles[region.triangle], region);
            const int x = TileX(tile), y = TileY(tile);
            const auto column = static_cast<int>(tile % columns);
            for (const auto& wide : wideRows[tile / columns]) {
                if (!wide.columns.Contains(column)) continue;
                const auto& triangle = triangles[wide.id];
                VisitAxis(triangle.top, triangle.bottom, y, std::min(y + RasterTileSize, height), height, [&](int top, int bottom) {
                    VisitAxis(triangle.left, triangle.right, x, std::min(x + RasterTileSize, width), width, [&](int left, int right) {
                        visit(triangle, RasterRegion{wide.id, left, top, right, bottom});
                    });
                });
            }
        }

    private:
        // A wrapped interval covers at most two canonical ranges. A span of
        // one full period or more touches every tile, regardless of its start.
        static TileFootprint Footprint(int begin, int end, int extent) {
            const int lastTile = (extent - 1) / RasterTileSize;
            const int span = end - begin;
            if (span >= extent) return {0, lastTile, -1};
            const int start = begin - RasterFloorDiv(begin, extent) * extent;
            const int finish = start + span;
            return {start / RasterTileSize, (std::min(finish, extent) - 1) / RasterTileSize,
                finish > extent ? (finish - extent - 1) / RasterTileSize : -1};
        }

        template <class Visit>
        static void WalkAxis(int begin, int end, int extent, Visit&& visit) {
            while (begin < end) {
                const int offset = RasterFloorDiv(begin, extent) * extent;
                const int tile = (begin - offset) / RasterTileSize;
                const int next = std::min(end, offset + std::min((tile + 1) * RasterTileSize, extent));
                visit(tile, begin, next);
                begin = next;
            }
        }

        template <class Visit>
        static void VisitAxis(int begin, int end, int tileBegin, int tileEnd, int extent, Visit&& visit) {
            int offset = RasterFloorDiv(begin - tileEnd, extent) * extent + extent;
            for (; tileBegin + offset < end; offset += extent)
                visit(std::max(begin, tileBegin + offset), std::min(end, tileEnd + offset));
        }

        int width, height, columns;
        std::vector<RasterTriangle> triangles;
        std::vector<std::vector<WideTriangle>> wideRows;
        std::vector<std::vector<RasterRegion>> bins;
    };
}
