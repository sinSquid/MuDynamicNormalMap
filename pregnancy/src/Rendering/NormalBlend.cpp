// SPDX-License-Identifier: GPL-3.0-only
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <d3dcompiler.h>
#include "Rendering/NormalBlend.h"

namespace PNS::Rendering {
    std::vector<std::uint8_t> RasterizePregnancyMask(std::span<const MaskVertex> vertices,
        std::span<const std::uint16_t> indices, const PregnancyRegion& region, unsigned size) {
        if (vertices.empty() || size < 2 || size > 2048) return {};
        float low = std::numeric_limits<float>::max(), high = -low;
        for (const auto& v : vertices) {
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
                !std::isfinite(v.u) || !std::isfinite(v.v)) return {};
            low = std::min(low, v.z); high = std::max(high, v.z);
        }
        const float height = high - low;
        if (height < 0.001f || region.radiusHeight <= 0 || region.radiusWidth <= 0) return {};
        std::vector<std::uint8_t> pixels(size * size, 0);
        auto edge = [](float ax, float ay, float bx, float by, float px, float py) {
            return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
        };
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
            if (indices[i] >= vertices.size() || indices[i+1] >= vertices.size() || indices[i+2] >= vertices.size()) continue;
            const auto& a = vertices[indices[i]]; const auto& b = vertices[indices[i+1]]; const auto& c = vertices[indices[i+2]];
            const float area = edge(a.u, a.v, b.u, b.v, c.u, c.v);
            if (std::abs(area) < 1e-10f) continue;
            int x0 = static_cast<int>(std::floor(std::clamp(std::min({a.u,b.u,c.u}), 0.0f, 1.0f) * size));
            int x1 = static_cast<int>(std::ceil(std::clamp(std::max({a.u,b.u,c.u}), 0.0f, 1.0f) * size));
            int y0 = static_cast<int>(std::floor(std::clamp(std::min({a.v,b.v,c.v}), 0.0f, 1.0f) * size));
            int y1 = static_cast<int>(std::ceil(std::clamp(std::max({a.v,b.v,c.v}), 0.0f, 1.0f) * size));
            for (int y = y0; y < std::min(y1, static_cast<int>(size)); ++y) for (int x = x0; x < std::min(x1, static_cast<int>(size)); ++x) {
                const float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
                const float w0 = edge(b.u,b.v,c.u,c.v,u,v) / area;
                const float w1 = edge(c.u,c.v,a.u,a.v,u,v) / area;
                const float w2 = 1.0f - w0 - w1;
                if (std::min({w0,w1,w2}) < -1e-5f) continue;
                const float px = a.x*w0+b.x*w1+c.x*w2;
                const float py = a.y*w0+b.y*w1+c.y*w2;
                const float pz = a.z*w0+b.z*w1+c.z*w2;
                const float dx = px / (height * region.radiusWidth);
                const float dz = ((pz-low)/height-region.centerHeight) / region.radiusHeight;
                const float breastX = (std::abs(px) / height - region.breastCenterOffset) / region.breastRadiusWidth;
                const float breastZ = ((pz-low)/height - region.breastCenterHeight) / region.breastRadiusHeight;
                float t = std::clamp(1.0f - std::min(dx*dx + dz*dz, breastX*breastX + breastZ*breastZ), 0.0f, 1.0f);
                t = t*t*(3.0f-2.0f*t);
                // Fade to zero at the sides and back; no hard seam at the midline.
                const float front = std::clamp(py * region.frontSign / (height * 0.025f), 0.0f, 1.0f);
                const auto value = static_cast<std::uint8_t>(std::lround(t * front * 255));
                auto& pixel = pixels[y*size+x]; pixel = std::max(pixel, value);
            }
        }
        if (std::none_of(pixels.begin(), pixels.end(), [](auto p) { return p != 0; })) return {};
        return pixels;
    }

    // Both model-space (_msn) and tangent-space RGB normals are interpolated as vectors.
    // Alpha (skin specularity) is retained; procedural smoothing never flattens to +Z.
    static constexpr char kShader[] = R"(
Texture2D<float4> Base : register(t0);
Texture2D<float4> Target : register(t1);
Texture2D<float> Mask : register(t2);
Texture2D<float4> SkinDetail : register(t3);
RWTexture2D<float4> Output : register(u0);
SamplerState LinearClamp : register(s0);
cbuffer Settings : register(b0) {
    float Amount; float Smoothing; float BlurRadius; uint HasTarget;
    uint Width; uint Height; uint ReplaceRegion; uint Encodings;
};
float3 unitNormal(float3 n, float3 fallback) {
    float len = dot(n,n); return len > 0.000001 ? n * rsqrt(len) : fallback;
}
float4 unpackSkin(float4 s, uint encoding, float alpha) {
    if (encoding == 0) return s;
    float2 xy = encoding == 1 ? s.rg * 2 - 1 : s.rg;
    return float4(float3(xy, sqrt(saturate(1 - dot(xy,xy)))) * 0.5 + 0.5, alpha);
}
[numthreads(8,8,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if(id.x >= Width || id.y >= Height) return;
    float2 uv = (float2(id.xy)+0.5)/float2(Width,Height);
    float4 source = Base.SampleLevel(LinearClamp,uv,0);
    float3 original = unitNormal(source.rgb*2-1,float3(0,0,1));
    float weight = saturate(Amount) * Mask.SampleLevel(LinearClamp,uv,0);
    if (weight <= 0) { Output[id.xy] = source; return; }
    uint skinEncoding = Encodings & 3;
    float4 skin = unpackSkin(SkinDetail.SampleLevel(LinearClamp,uv,0),skinEncoding,source.a);
    float3 skinNormal = unitNormal(skin.rgb*2-1,original);
    float4 destination;
    if (HasTarget != 0) destination = unpackSkin(Target.SampleLevel(LinearClamp,uv,0),Encodings >> 2,skin.a);
    else {
        float3 sum = 0;
        [unroll] for(int y=-2;y<=2;y++) [unroll] for(int x=-2;x<=2;x++)
            sum += unpackSkin(SkinDetail.SampleLevel(LinearClamp,uv+float2(x,y)*BlurRadius*0.5,0),skinEncoding,source.a).rgb*2-1;
        destination = float4(unitNormal(sum,skinNormal)*0.5+0.5,skin.a);
        if (ReplaceRegion != 0) destination.rgb = lerp(skin.rgb, destination.rgb, Smoothing);
        else weight *= Smoothing;
    }
    if (ReplaceRegion != 0) {
        float3 n = unitNormal(lerp(original,unitNormal(destination.rgb*2-1,skinNormal),weight),original);
        Output[id.xy] = float4(n*0.5+0.5,lerp(source.a,destination.a,weight));
        return;
    }
    float3 delta = unitNormal(destination.rgb*2-1,skinNormal) - skinNormal;
    float3 n = unitNormal(original + weight*delta, original);
    Output[id.xy] = float4(n*0.5+0.5,saturate(source.a + weight*(destination.a-skin.a)));
})";

    bool NormalBlend::Check(HRESULT result, const char* operation) {
        if (SUCCEEDED(result)) return true;
        error_ = std::string(operation) + " failed (HRESULT " + std::to_string(static_cast<unsigned>(result)) + ")";
        return false;
    }
    bool NormalBlend::Initialize(ID3D11Device* device) {
        if (!device) { error_ = "No D3D11 device"; return false; }
        device_ = device;
        static std::once_flag compileOnce;
        static ComPtr<ID3DBlob> code, errors;
        static HRESULT result = E_FAIL;
        std::call_once(compileOnce, [] {
            result = D3DCompile(kShader,sizeof(kShader)-1,"PNSNormalBlend",nullptr,nullptr,
                "main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
        });
        if (!Check(result,"Compile normal blend shader")) {
            if (errors) error_ += std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize());
            return false;
        }
        if (!Check(device_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader_), "Create shader") ||
            !Check(device_->CreateDeferredContext(0, &deferred_), "Create deferred context")) return false;
        D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth = 32; buffer.Usage = D3D11_USAGE_DEFAULT; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (!Check(device_->CreateBuffer(&buffer,nullptr,&constants_), "Create constants")) return false;
        D3D11_SAMPLER_DESC sampler{}; sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sampler.MaxLOD = D3D11_FLOAT32_MAX;
        return Check(device_->CreateSamplerState(&sampler,&sampler_), "Create sampler");
    }
    bool NormalBlend::CreateOutput(unsigned width, unsigned height) {
        if (!device_ || width == 0 || height == 0 || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) return false;
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height; desc.MipLevels = 0;
        desc.ArraySize = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        // Build transactionally: failures leave the previous valid output intact.
        ComPtr<ID3D11Texture2D> output; ComPtr<ID3D11ShaderResourceView> view; ComPtr<ID3D11UnorderedAccessView> uav;
        if (!Check(device_->CreateTexture2D(&desc,nullptr,&output),"Create output") ||
            !Check(device_->CreateShaderResourceView(output.Get(),nullptr,&view),"Create output SRV") ||
            !Check(device_->CreateUnorderedAccessView(output.Get(),nullptr,&uav),"Create output UAV")) return false;
        output_ = std::move(output); view_ = std::move(view); uav_ = std::move(uav);
        width_ = width; height_ = height;
        return true;
    }
    bool NormalBlend::SetMask(std::span<const std::uint8_t> pixels, unsigned size) {
        if (!device_ || size == 0 || pixels.size() != size_t(size)*size) return false;
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = desc.Height = size; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels.data(),size,0}; ComPtr<ID3D11Texture2D> texture;
        return Check(device_->CreateTexture2D(&desc,&data,&texture),"Create belly mask") &&
            Check(device_->CreateShaderResourceView(texture.Get(),nullptr,mask_.ReleaseAndGetAddressOf()),"Create mask SRV");
    }
    bool NormalBlend::Render(ID3D11DeviceContext* immediate, ID3D11ShaderResourceView* base,
        ID3D11ShaderResourceView* target, const BlendSettings& settings, ID3D11ShaderResourceView* skinDetail) {
        if (!immediate || !base || !uav_ || !mask_ || !shader_) return false;
        auto encoding = [](ID3D11ShaderResourceView* view) {
            D3D11_SHADER_RESOURCE_VIEW_DESC d{};
            if (view) view->GetDesc(&d);
            if (d.Format == DXGI_FORMAT_BC5_UNORM) return 1u;
            if (d.Format == DXGI_FORMAT_BC5_SNORM) return 2u;
            return 0u;
        };
        struct Constants { float amount, smoothing, blurRadius; unsigned hasTarget, width, height, replaceRegion, encodings; };
        const Constants values{settings.amount,settings.smoothing,settings.blurRadius,settings.hasTarget && target ? 1u:0u,width_,height_,settings.replaceRegion ? 1u:0u,encoding(skinDetail) | (encoding(target)<<2)};
        deferred_->UpdateSubresource(constants_.Get(),0,nullptr,&values,0,0);
        ID3D11ShaderResourceView* inputs[]{base,target ? target : base,mask_.Get(),skinDetail ? skinDetail : base};
        auto* buffer = constants_.Get(); auto* sampler = sampler_.Get(); auto* uav = uav_.Get();
        deferred_->CSSetShader(shader_.Get(),nullptr,0); deferred_->CSSetShaderResources(0,4,inputs);
        deferred_->CSSetConstantBuffers(0,1,&buffer); deferred_->CSSetSamplers(0,1,&sampler);
        deferred_->CSSetUnorderedAccessViews(0,1,&uav,nullptr);
        deferred_->Dispatch((width_+7)/8,(height_+7)/8,1);
        ID3D11UnorderedAccessView* noUAV = nullptr;
        deferred_->CSSetUnorderedAccessViews(0,1,&noUAV,nullptr);
        deferred_->GenerateMips(view_.Get());
        ComPtr<ID3D11CommandList> commands;
        if (!Check(deferred_->FinishCommandList(FALSE,&commands),"Finish texture commands")) return false;
        immediate->ExecuteCommandList(commands.Get(),TRUE); // Preserve all engine context state.
        return true;
    }
}

