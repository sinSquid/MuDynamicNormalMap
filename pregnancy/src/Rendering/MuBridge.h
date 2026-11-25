#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <span>
#include <unordered_set>

namespace PNS {
    bool HasDinoBridge();
    // Main/game thread only. Restore never writes into a shared NiTexture wrapper.
    bool HasInstalledLayer(RE::FormID actorID);
    bool OwnsInstalledGeometry(RE::FormID actorID, RE::BSGeometry* geometry);
    // Caller supplies a current/live geometry. Restore only our debug diffuse,
    // then retire that geometry's resource references after a successful swap.
    bool RetireInstalledGeometry(RE::FormID actorID, RE::BSGeometry* geometry);
    bool RestoreInstalledLayer(RE::Actor* actor);
    RE::NiSourceTexturePtr ResolveRestoredBase(RE::NiSourceTexture* savedBase);
    void RestoreDisabledDebugLayers();
    void ForgetInstalledLayer(RE::FormID actorID);
    void ClearInstalledLayers();
    void PruneInstalledLayers(RE::FormID actorID, const std::unordered_set<RE::BSGeometry*>& live);
    void ForgetInstalledGeometry(RE::FormID actorID, RE::BSGeometry* geometry);
    void OnInstalledMaterialCloned(RE::FormID actorID, RE::BSGeometry* geometry,
        RE::BSLightingShaderMaterialBase* source, RE::BSLightingShaderMaterialBase* replacement);
    void RecordInstalledLayer(RE::Actor* actor, RE::BSGeometry* geometry,
        RE::BSLightingShaderMaterialBase* material, RE::NiSourceTexture* installed,
        RE::NiSourceTexture* base, RE::NiSourceTexture* debugDiffuse, RE::NiSourceTexture* originalDiffuse);
    RE::NiSourceTexturePtr CreatePrivateNormalTexture(const std::string& sourceName,
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view);

    RE::BSLightingShaderMaterialBase* PrivateNormalMaterial(RE::FormID actorID, RE::BSGeometry* geometry);
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

