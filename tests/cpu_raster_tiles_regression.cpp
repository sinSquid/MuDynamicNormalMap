#define NOMINMAX
#include "../include/CPURasterTiles.h"
#include "../include/NormalMath.h"
#include <array>
#include <cassert>
#include <limits>
#include <random>
#include <iostream>
using namespace Mus;
struct Pixel { unsigned owner=0;int x=0,y=0;bool operator==(const Pixel&)const=default;};
bool ReferenceCoverage(const RasterTriangle& t, float x, float y) {
    using namespace DirectX;
    const auto a = XMVectorSet(static_cast<float>(t.p1.x - t.p0.x), static_cast<float>(t.p1.y - t.p0.y), 0, 0);
    const auto b = XMVectorSet(static_cast<float>(t.p2.x - t.p0.x), static_cast<float>(t.p2.y - t.p0.y), 0, 0);
    const auto p = XMVectorSet(x - t.p0.x, y - t.p0.y, 0, 0);
    const float aa = XMVectorGetX(XMVector2Dot(a, a));
    const float ab = XMVectorGetX(XMVector2Dot(a, b));
    const float bb = XMVectorGetX(XMVector2Dot(b, b));
    const float pa = XMVectorGetX(XMVector2Dot(p, a));
    const float pb = XMVectorGetX(XMVector2Dot(p, b));
    const float denominator = aa * bb - ab * ab;
    if (denominator == 0.0f) return false;
    const float v = (bb * pa - ab * pb) / denominator;
    const float w = (aa * pb - ab * pa) / denominator;
    return 1.0f - v - w >= 0 && v >= 0 && w >= 0;
}


struct UVTriangle {
    DirectX::XMFLOAT2 a, b, c;
};

std::uint64_t CompareRaster(int width, int height, const std::vector<UVTriangle>& input)
{
    CPURasterTiles tiles(width, height);
    std::vector<Pixel> expected(width * height), actual(width * height);
    for (std::uint32_t i = 0; i < input.size(); ++i) {
        const auto& uv = input[i];
        RasterTriangle reference{};
        const auto project = [&](DirectX::XMFLOAT2 v) {
            return DirectX::XMINT2{static_cast<int>(v.x * width), static_cast<int>(v.y * height)};
        };
        reference.p0 = project(uv.a);
        reference.p1 = project(uv.b);
        reference.p2 = project(uv.c);
        const int left = std::min({reference.p0.x, reference.p1.x, reference.p2.x});
        const int right = std::max({reference.p0.x, reference.p1.x, reference.p2.x});
        const int top = std::min({reference.p0.y, reference.p1.y, reference.p2.y});
        const int bottom = std::max({reference.p0.y, reference.p1.y, reference.p2.y});
        for (int y = top; y <= bottom; ++y)
            for (int x = left; x <= right; ++x)
                if (ReferenceCoverage(reference, x + .5f, y + .5f))
                    expected[WrapTextureCoordinate(y, height) * width + WrapTextureCoordinate(x, width)] = {i + 1, x, y};

        RasterTriangle projected{};
        if (ProjectRasterTriangle(projected, uv.a, uv.b, uv.c, width, height, i * 3))
            tiles.Add(projected);
    }

    std::uint64_t skippedCoverage = 0;
    std::vector<bool> scheduled(((width + 31) / 32) * ((height + 31) / 32));
    for (std::size_t position = 0; position < tiles.ScheduledCount(); ++position) {
        const auto tile = tiles.ScheduledTile(position);
        assert(tile < scheduled.size() && !scheduled[tile]);
        scheduled[tile] = true;
        if (!tiles.HasRegions(tile)) continue;
        const auto tileX = tiles.TileX(tile), tileY = tiles.TileY(tile);
        std::array<unsigned, RasterTileSize * RasterTileSize> owners{};
        for (bool shade : {false, true}) {
            tiles.VisitRegions(tile, [&](const auto& triangle, const auto& region) {
                for (int y = region.top; y < region.bottom; ++y) {
                    for (int x = region.left; x < region.right; ++x) {
                        const auto wx = WrapTextureCoordinate(x, width);
                        const auto wy = WrapTextureCoordinate(y, height);
                        assert(wx >= tileX && wx < tileX + RasterTileSize);
                        assert(wy >= tileY && wy < tileY + RasterTileSize);
                        auto& owner = owners[(wy - tileY) * RasterTileSize + wx - tileX];
                        const auto priority = triangle.index / 3 + 1;
                        if (shade && owner != priority) {
                            ++skippedCoverage;
                            continue;
                        }
                        DirectX::XMFLOAT3 bary;
                        if (!triangle.Covers(x + .5f, y + .5f, bary)) continue;
                        if (!shade) owner = std::max(owner, priority);
                        else actual[wy * width + wx] = {owner, x, y};
                    }
                }
            });
        }
    }
    assert(actual == expected);
    return skippedCoverage;
}

int main()
{
    CompareRaster(64, 64, {{{0, 0}, {3, 0}, {0, 1}}});
    std::vector<UVTriangle> input = {
        {{-.25f, -.3f}, {1.25f, .5f}, {.8f, 1.15f}},
        {{-1.f, .1f}, {0.f, .1f}, {-.2f, 1.f}},
        {{0.f, .1f}, {1.f, .1f}, {.8f, 1.f}},
        {{0.f, .1f}, {1.f, .1f}, {.8f, 1.f}},
        {{-2.4f, -1.7f}, {1.3f, -1.4f}, {.8f, 2.1f}}
    };
    std::mt19937 rng(193);
    std::uniform_real_distribution<float> coord(-1.5f, 1.5f);
    for (int i = 0; i < 36; ++i) {
        const float x = coord(rng), y = coord(rng);
        input.push_back({{x, y}, {x + .17f, y + .22f}, {x - .09f, y + .25f}});
    }
    for (auto [width, height] : {std::pair{31, 31}, {37, 67}, {65, 32}, {613, 477}})
        CompareRaster(width, height, input);

    constexpr int extent = 16384;
    RasterTriangle wide{};
    assert(ProjectRasterTriangle(wide, {-500.f / extent, -500.f / extent},
        {.5f / extent, -500.f / extent}, {-500.f / extent, .5f / extent}, extent, extent, 0));
    CPURasterTiles sparse(extent, extent);
    sparse.Add(wide);
    sparse.Add(wide);
    std::size_t active = 0;
    assert(sparse.ScheduledCount() == 289);
    std::vector<bool> scheduled(512 * 512);
    for (std::size_t position = 0; position < sparse.ScheduledCount(); ++position) {
        const auto tile = sparse.ScheduledTile(position);
        assert(tile < scheduled.size() && !scheduled[tile]);
        scheduled[tile] = true;
        bool visited = false;
        if (sparse.HasRegions(tile))
            sparse.VisitRegions(tile, [&](const auto&, const auto&) { visited = true; });
        assert(visited == sparse.HasRegions(tile));
        active += visited;
    }
    assert(active == 17 * 17);

    const UVTriangle full{{0, 0}, {1, 0}, {0, 1}};
    const auto skipped = CompareRaster(128, 128, {full, full});
    assert(skipped >= 128 * 128);
    CPURasterTiles dense(256, 256);
    assert(ProjectRasterTriangle(wide, full.a, full.b, full.c, 256, 256, 0));
    dense.Add(wide);
    assert(dense.ScheduledCount() == 64);
    for (std::size_t i = 0; i < dense.ScheduledCount(); ++i)
        assert(dense.ScheduledTile(i) == i && dense.HasRegions(i));

    RasterTriangle ignored{};
    assert(!ProjectRasterTriangle(ignored, {std::numeric_limits<float>::quiet_NaN(), 0}, {0, 0}, {1, 1}, 64, 64, 0));
    assert(!ProjectRasterTriangle(ignored, {1e20f, 0}, {0, 0}, {1, 1}, 64, 64, 0));

    auto batch = RasterBatchSampleLimit;
    auto geometry = RasterGeometrySampleLimit(256, 256);
    RasterTriangle extreme{};
    assert(ProjectRasterTriangle(extreme, {-65504, 0}, {65504, 0}, {0, 65504}, 256, 256, 0));
    const auto initialGeometryBudget = geometry;
    assert(!ReserveRasterSamples(extreme, 256, 256, geometry, batch));
    assert(geometry == initialGeometryBudget && batch == RasterBatchSampleLimit);

    RasterTriangle narrow{};
    assert(ProjectRasterTriangle(narrow, {0, 0}, {1.f / extent, 0}, {0, .25f}, extent, extent, 0));
    assert(UsesWideRasterBins(narrow));
    geometry = static_cast<std::uint64_t>(narrow.right - narrow.left) * (narrow.bottom - narrow.top);
    // The old bounding-box-only estimate would fit, but the wide-row lookups
    // have real work even for tiles outside this triangle's narrow X range.
    assert(!ReserveRasterSamples(narrow, extent, extent, geometry, batch));

    for (int size : {2048, 4096}) {
        RasterTriangle tiled{};
        assert(ProjectRasterTriangle(tiled, {0, 0}, {3, 0}, {0, 3}, size, size, 0));
        auto remaining = RasterGeometrySampleLimit(size, size);
        auto remainingBatch = RasterBatchSampleLimit;
        assert(ReserveRasterSamples(tiled, size, size, remaining, remainingBatch));
    }

    RasterTriangle repeated{};
    assert(ProjectRasterTriangle(repeated, {0, 0}, {3, 0}, {0, 1}, 64, 64, 0));
    const auto cost = static_cast<std::uint64_t>(repeated.right - repeated.left) * (repeated.bottom - repeated.top);
    geometry = 2 * cost;
    assert(ReserveRasterSamples(repeated, 64, 64, geometry, batch));
    assert(ReserveRasterSamples(repeated, 64, 64, geometry, batch));
    assert(!ReserveRasterSamples(repeated, 64, 64, geometry, batch));
    assert(geometry == 0 && batch == RasterBatchSampleLimit - 2 * cost);
    geometry = 2 * cost;
    batch = cost;
    assert(ReserveRasterSamples(repeated, 64, 64, geometry, batch));
    assert(!ReserveRasterSamples(repeated, 64, 64, geometry, batch));
    assert(geometry == cost && batch == 0);
    std::cout << "PASS: sparse scheduled=289/262144, dense sequential, overlap coverage calls avoided="
              << skipped << ", multi-period UV, geometry/batch budgets and extreme input\n";
}
