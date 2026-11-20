#pragma once

#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace PNS::Rendering {
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    inline float NormalizeMorph(float value, float minimum, float maximum) {
        if (!std::isfinite(value) || !std::isfinite(minimum) ||
            !std::isfinite(maximum) || maximum <= minimum) return 0.0f;
        return std::clamp((value - minimum) / (maximum - minimum), 0.0f, 1.0f);
    }

    struct MaskVertex { float x, y, z, u, v; };
    struct PregnancyRegion {
        // Fractions of the body's model-space height. Positive Y points forward on the reference CBBE/3BA meshes.
        float centerHeight = 0.69f;
        float radiusHeight = 0.12f;
        float radiusWidth = 0.105f;
        float frontSign = 1.0f;
        float breastCenterHeight = 0.83f;
        float breastCenterOffset = 0.07f;
        float breastRadiusHeight = 0.065f;
        float breastRadiusWidth = 0.063f;
    };
    std::vector<std::uint8_t> RasterizePregnancyMask(std::span<const MaskVertex> vertices,
        std::span<const std::uint16_t> indices, const PregnancyRegion& region, unsigned size = 256);

    struct BlendSettings {
        float amount = 0.0f;
        float smoothing = 0.85f;
        float blurRadius = 0.008f; // UV distance, independent of texture resolution
        bool hasTarget = false;
        bool replaceRegion = false;
    };

    // Uses a deferred context. The caller serializes ExecuteCommandList with the renderer.
    class NormalBlend {
    public:
        bool Initialize(ID3D11Device* device);
        bool CreateOutput(unsigned width, unsigned height);
        bool SetMask(std::span<const std::uint8_t> pixels, unsigned size);
        bool Render(ID3D11DeviceContext* immediate, ID3D11ShaderResourceView* base,
            ID3D11ShaderResourceView* target, const BlendSettings& settings,
            ID3D11ShaderResourceView* skinDetail = nullptr);
        ID3D11Texture2D* Texture() const { return output_.Get(); }
        ID3D11ShaderResourceView* View() const { return view_.Get(); }
        const std::string& Error() const { return error_; }
    private:
        bool Check(HRESULT result, const char* operation);
        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> deferred_;
        ComPtr<ID3D11ComputeShader> shader_;
        ComPtr<ID3D11Buffer> constants_;
        ComPtr<ID3D11SamplerState> sampler_;
        ComPtr<ID3D11Texture2D> output_;
        ComPtr<ID3D11ShaderResourceView> view_, mask_;
        ComPtr<ID3D11UnorderedAccessView> uav_;
        unsigned width_ = 0, height_ = 0;
        std::string error_;
    };
}
