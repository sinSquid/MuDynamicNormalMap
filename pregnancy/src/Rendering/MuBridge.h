#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <span>

namespace PNS {
    bool HasDinoBridge();
    RE::BSLightingShaderMaterialBase* PrivateNormalMaterial(RE::BSGeometry* geometry);
    bool ReadAssignedNormal(const std::string& path, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view);
    unsigned MuUpdateDelayFrames();
    bool QueueMuSlots(RE::Actor* actor, std::uint32_t slots);
    bool QueueMuBody(RE::Actor* actor);
    // The result geometry is an identity token from a background job.
    // Resolve it against the current 3D before reading mesh data.
    void ComposeMuBody(RE::Actor* actor, std::span<RE::BSGeometry* const> identities,
        Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view,
        std::string_view sourcePath = {}, bool dinoBase = false);
}

