#include "pch.h"
#include "Rendering/MuBridge.h"
#include "Rendering/InstalledLayerIdentity.h"
#include "Compat/SkinPaths.h"
#include "Lifecycle.h"

namespace PNS {
    namespace {
        struct InstalledLayer {
            // Strong references prevent recycled addresses and keep the saved base alive.
            RE::NiPointer<RE::BSGeometry> geometry;
            RE::BSTSmartPointer<RE::BSLightingShaderMaterialBase> material;
            RE::NiSourceTexturePtr installed, base, originalDiffuse;
            bool debugOverride = false;
            RE::NiPointer<RE::BSTextureSet> textureSet;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
            Rendering::InstalledLayerIdentity identity;
        };
        std::unordered_map<RE::FormID, std::vector<InstalledLayer>> layers;

        RE::BSLightingShaderMaterialBase* Material(RE::BSGeometry* geometry) {
            auto* shader = netimmerse_cast<RE::BSLightingShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get());
            return shader ? skyrim_cast<RE::BSLightingShaderMaterialBase*>(shader->material) : nullptr;
        }
        Rendering::InstalledLayerIdentity Identity(RE::BSGeometry* geometry,
            RE::BSLightingShaderMaterialBase* material) {
            Rendering::InstalledLayerIdentity result;
            result.geometry = reinterpret_cast<std::uintptr_t>(geometry);
            if (!material) return result;
            result.material = reinterpret_cast<std::uintptr_t>(material);
            result.texture = reinterpret_cast<std::uintptr_t>(material->normalTexture.get());
            result.textureSet = reinterpret_cast<std::uintptr_t>(material->textureSet.get());
            if (material->normalTexture && material->normalTexture->rendererTexture)
                result.view = reinterpret_cast<std::uintptr_t>(material->normalTexture->rendererTexture->resourceView);
            if (material->textureSet) {
                const char* path = material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kNormal);
                if (path) result.normalPath = SkinPaths::Relative(path);
            }
            return result;
        }
        void RestoreDebugDiffuse(const InstalledLayer& layer, RE::BSGeometry* geometry,
            RE::BSLightingShaderMaterialBase* material) {
            if (!layer.debugOverride || !material || !material->diffuseTexture) return;
            auto* diffuse = material->diffuseTexture.get();
            const auto view = diffuse->rendererTexture ?
                reinterpret_cast<std::uintptr_t>(diffuse->rendererTexture->resourceView) : 0;
            if (Rendering::OwnsDebugDiffuse(layer.identity, Identity(geometry, material),
                    reinterpret_cast<std::uintptr_t>(diffuse), view))
                material->diffuseTexture = layer.originalDiffuse;
        }
        std::unordered_set<RE::BSGeometry*> LiveShapes(RE::Actor* actor) {
            std::unordered_set<RE::BSGeometry*> result;
            if (actor && actor->Get3D())
                RE::BSVisit::TraverseScenegraphGeometries(actor->Get3D(), [&](RE::BSGeometry* geometry) {
                    result.insert(geometry);
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            return result;
        }
    }

    bool HasInstalledLayer(RE::FormID actorID) {
        const auto found = layers.find(actorID);
        return found != layers.end() && !found->second.empty();
    }
    bool OwnsInstalledGeometry(RE::FormID actorID, RE::BSGeometry* geometry) {
        if (!geometry) return false;
        const auto found = layers.find(actorID);
        if (found == layers.end()) return false;
        const auto current = Identity(geometry, Material(geometry));
        return std::ranges::any_of(found->second, [&](const auto& layer) {
            return Rendering::OwnsInstalledLayer(layer.identity, current);
        });
    }
    bool RetireInstalledGeometry(RE::FormID actorID, RE::BSGeometry* geometry) {
        if (!geometry) return false;
        const auto found = layers.find(actorID);
        if (found == layers.end()) return false;
        auto* material = Material(geometry);
        bool retired = false;
        for (const auto& layer : found->second) {
            if (layer.geometry.get() != geometry) continue;
            RestoreDebugDiffuse(layer, geometry, material);
            retired = true;
        }
        ForgetInstalledGeometry(actorID, geometry);
        return retired;
    }
    void ForgetInstalledLayer(RE::FormID actorID) { layers.erase(actorID); }
    void ClearInstalledLayers() { layers.clear(); }
    void PruneInstalledLayers(RE::FormID actorID, const std::unordered_set<RE::BSGeometry*>& live) {
        const auto found = layers.find(actorID);
        if (found == layers.end()) return;
        std::erase_if(found->second, [&](const auto& layer) { return !live.contains(layer.geometry.get()); });
        if (found->second.empty()) layers.erase(found);
    }
    void ForgetInstalledGeometry(RE::FormID actorID, RE::BSGeometry* geometry) {
        const auto found = layers.find(actorID);
        if (found == layers.end()) return;
        std::erase_if(found->second, [&](const auto& layer) { return layer.geometry.get() == geometry; });
        if (found->second.empty()) layers.erase(found);
    }

    void OnInstalledMaterialCloned(RE::FormID actorID, RE::BSGeometry* geometry,
        RE::BSLightingShaderMaterialBase* source, RE::BSLightingShaderMaterialBase* replacement) {
        const auto found = layers.find(actorID);
        if (found == layers.end() || !geometry || !source || !replacement) return;
        const auto before = Identity(geometry, source);
        auto after = Identity(geometry, replacement);
        auto comparable = after;
        comparable.material = before.material;
        for (auto& layer : found->second) {
            const auto* diffuse = source->diffuseTexture.get();
            const bool ownsDebug = layer.debugOverride && diffuse &&
                replacement->diffuseTexture == source->diffuseTexture &&
                Rendering::OwnsDebugDiffuse(layer.identity, before,
                    reinterpret_cast<std::uintptr_t>(diffuse), diffuse->rendererTexture ?
                    reinterpret_cast<std::uintptr_t>(diffuse->rendererTexture->resourceView) : 0);
            if ((Rendering::OwnsInstalledLayer(layer.identity, before) || ownsDebug) && comparable == before) {
                layer.material.reset(replacement);
                // A foreign normal may coexist with our debug diffuse. Migrate
                // only our material identity; never claim that foreign normal.
                layer.identity.material = after.material;
                break;
            }
        }
    }

    void RecordInstalledLayer(RE::Actor* actor, RE::BSGeometry* geometry,
        RE::BSLightingShaderMaterialBase* material, RE::NiSourceTexture* installed, RE::NiSourceTexture* base, bool debugOverride, RE::NiSourceTexture* originalDiffuse) {
        if (!actor || !geometry || !material || material->normalTexture.get() != installed ||
            !installed || !installed->rendererTexture || !installed->rendererTexture->resourceView ||
            !base || !base->rendererTexture || !base->rendererTexture->resourceView ||
            installed->rendererTexture->resourceView == base->rendererTexture->resourceView) return;
        InstalledLayer layer;
        layer.geometry.reset(geometry);
        layer.material.reset(material);
        layer.installed.reset(installed);
        layer.base.reset(base);
        layer.debugOverride = debugOverride;
        if (debugOverride) layer.originalDiffuse.reset(originalDiffuse);
        layer.textureSet = material->textureSet;
        layer.view = reinterpret_cast<ID3D11ShaderResourceView*>(installed->rendererTexture->resourceView);
        layer.identity = Identity(geometry, material);
        auto& actorLayers = layers[actor->GetFormID()];
        std::erase_if(actorLayers, [&](const auto& previous) {
            return previous.geometry.get() == geometry;
        });
        actorLayers.push_back(std::move(layer));
    }

    bool RestoreInstalledLayer(RE::Actor* actor) {
        if (!actor) return false;
        const auto found = layers.find(actor->GetFormID());
        if (found == layers.end()) return true;
        if (!SessionActive()) return false;
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer) return false;
        renderer->Lock();
        struct Unlock { RE::BSGraphics::Renderer* renderer; ~Unlock() { renderer->Unlock(); } } unlock{renderer};
        const auto live = LiveShapes(actor);
        std::erase_if(found->second, [&](const auto& layer) {
            // Never dereference a retired identity or overwrite an external material/
            // wrapper/SRV change. The saved geometry is pinned, then checked live.
            if (!live.contains(layer.geometry.get())) return true;
            auto* material = Material(layer.geometry.get());
            RestoreDebugDiffuse(layer, layer.geometry.get(), material);
            if (!Rendering::OwnsInstalledLayer(layer.identity, Identity(layer.geometry.get(), material))) return true;
            auto base = ResolveRestoredBase(layer.base.get());
            if (!base) return false;
            material->normalTexture = std::move(base);
            return true;
        });
        if (!found->second.empty()) return false;
        layers.erase(found);
        return true;
    }
}
