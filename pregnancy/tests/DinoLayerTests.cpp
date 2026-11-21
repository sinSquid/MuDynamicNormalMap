#include <Windows.h>
#include <d3d11.h>
#include <cstdint>
#include <stdexcept>
#include <string>
namespace RE { class BSGeometry; }
#include "../patches/dino/PregnancyBridge.h"

using namespace RMF::PregnancyBridge;
using Microsoft::WRL::ComPtr;
namespace {
    std::uint64_t strength = 0;
    unsigned compositions = 0;
    bool rejectComposition = false;
    std::string observedSkin;
    ComPtr<ID3D11Texture2D> overrideTexture;
    ComPtr<ID3D11ShaderResourceView> overrideView;
    void Check(bool ok) { if (!ok) throw std::runtime_error("Dino regional layer regression"); }
}

int main() {
    ComPtr<ID3D11Device> device;
    Check(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&device,nullptr,nullptr)));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 8;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> raw;
    ComPtr<ID3D11ShaderResourceView> rawView;
    Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&raw)));
    Check(SUCCEEDED(device->CreateShaderResourceView(raw.Get(),nullptr,&rawView)));
    Check(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&overrideTexture)));
    Check(SUCCEEDED(device->CreateShaderResourceView(overrideTexture.Get(),nullptr,&overrideView)));
    key = [](std::uint32_t id) { return id == 1 ? strength : std::uint64_t{0}; };
    compose = [](std::uint32_t,RE::BSGeometry*,const char* skin,ID3D11Texture2D*,ID3D11ShaderResourceView*,
                 ID3D11Texture2D** tex,ID3D11ShaderResourceView** view) {
        ++compositions; observedSkin = skin;
        if (rejectComposition) return false;
        overrideTexture.CopyTo(tex); overrideView.CopyTo(view);
        return true;
    };
    Layer layer, other;
    layer.Capture(raw.Get(),rawView.Get());
    other.Capture(raw.Get(),rawView.Get());
    auto* geometry = reinterpret_cast<RE::BSGeometry*>(1);
    ComPtr<ID3D11Texture2D> visible;
    ComPtr<ID3D11ShaderResourceView> visibleView;
    unsigned swaps = 0;
    auto install = [&](auto* tex, auto* view, unsigned size, unsigned mips) {
        ++swaps; Check(size == 8 && mips == 1);
        visible.Attach(tex); visibleView.Attach(view);
    };
    layer.Update(1,geometry,"packA",install);
    Check(!swaps && !compositions);
    strength = 1;
    layer.Update(1,geometry,"packA",install);
    layer.Update(1,geometry,"packA",install);
    other.Update(2,geometry,"packB",install);
    Check(swaps == 1 && compositions == 1 && visible.Get() == overrideTexture.Get() && observedSkin == "packA");
    layer.Capture(raw.Get(),rawView.Get());
    layer.Update(1,geometry,"packB",install);
    Check(swaps == 2 && observedSkin == "packB");
    strength = 0;
    layer.Update(1,geometry,"packB",install);
    Check(swaps == 3 && compositions == 2 && visible.Get() == raw.Get() && visibleView.Get() == rawView.Get());
    strength = 0;
    layer.Update(1,geometry,"packB",install);
    Check(swaps == 3 && compositions == 2);
    strength = 2;
    rejectComposition = true;
    layer.Update(1,geometry,"packB",install);
    Check(swaps == 3 && compositions == 3);
    rejectComposition = false;
    layer.Update(1,geometry,"packB",install);
    Check(swaps == 4 && compositions == 4 && visible.Get() == overrideTexture.Get());
    strength = 3;
    rejectComposition = true;
    layer.Update(1,geometry,"packB",install);
    Check(swaps == 5 && compositions == 5 && visible.Get() == raw.Get());
    rejectComposition = false;
    layer.Update(1,geometry,"packB",install);
    Check(swaps == 6 && compositions == 6 && visible.Get() == overrideTexture.Get());
}

