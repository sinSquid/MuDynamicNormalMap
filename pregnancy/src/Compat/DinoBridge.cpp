#include "pch.h"
#include "Actor/ActorManager.h"
#include "Rendering/MuBridge.h"
#include <bit>
#include "Lifecycle.h"

bool PNS::HasDinoBridge() {
    if (!ModuleEnabled() || !SessionActive()) return false;
    // First queried during gameplay, after SKSE finishes loading plugins.
    static const bool available = [] {
        auto module = GetModuleHandleW(L"DinoRMF.dll");
        return module && GetProcAddress(module, "DinoRMF_PregnancyBridgeV1");
    }();
    return available;
}

// Game-thread ABI. Both input resources are borrowed. Successful composition
// returns one owned COM reference per output. An unchanged bake needs no output.
extern "C" __declspec(dllexport) std::uint64_t PNS_DinoLayerKeyV1(std::uint32_t actor) noexcept {
    if (!PNS::ModuleEnabled() || !PNS::SessionActive()) return 0;
    try {
        auto [amount, stage] = PNS::ActorManager::GetSingleton().GetPregnancyLayer(actor);
        if (amount <= 0) return 0;
        return (std::uint64_t(std::bit_cast<std::uint32_t>(amount)) << 32) |
            static_cast<std::uint32_t>(std::hash<std::string>{}(stage));
    } catch (...) { return 0; }
}

extern "C" __declspec(dllexport) bool PNS_DinoComposeV1(std::uint32_t id, RE::BSGeometry* geometry,
    const char* sourcePath, ID3D11Texture2D* input, ID3D11ShaderResourceView* inputView,
    ID3D11Texture2D** output, ID3D11ShaderResourceView** outputView) noexcept {
    if (!output || !outputView) return false;
    *output = nullptr;
    *outputView = nullptr;
    if (!PNS::ModuleEnabled() || !PNS::SessionActive() || !geometry || !input || !inputView) return false;
    try {
        auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
        if (!actor || !actor->Is3DLoaded()) return false;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture = input;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view = inputView;
        PNS::ComposeMuBody(actor, {&geometry, 1}, texture, view, sourcePath ? sourcePath : "", true);
        if (view.Get() == inputView) return false;
        *output = texture.Detach();
        *outputView = view.Detach();
        return true;
    } catch (...) { return false; }
}

