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
            RE::NiSourceTexturePtr debugDiffuse;
            std::string diffuseSource;
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
        std::string DiffuseSource(RE::BSLightingShaderMaterialBase* material) {
            const auto* path = material->textureSet ? material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr;
            return path ? SkinPaths::Relative(path) : std::string{};
        }
        bool OwnsDebug(const InstalledLayer& layer, RE::BSGeometry* geometry, RE::BSLightingShaderMaterialBase* material) {
            return layer.debugDiffuse && material && layer.geometry.get() == geometry && layer.material.get() == material &&
                material->diffuseTexture == layer.debugDiffuse && layer.textureSet.get() == material->textureSet.get() &&
                layer.debugDiffuse->rendererTexture && layer.debugDiffuse->rendererTexture->resourceView ==
                    reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(layer.view.Get()) &&
                layer.diffuseSource == DiffuseSource(material);
        }
        void RestoreDebugDiffuse(const InstalledLayer& layer, RE::BSGeometry* geometry,
            RE::BSLightingShaderMaterialBase* material) {
            if (OwnsDebug(layer, geometry, material)) material->diffuseTexture = layer.originalDiffuse;
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
        return found != layers.end() && std::ranges::any_of(found->second, [](const auto& layer) { return bool(layer.base); });
    }
    bool OwnsInstalledGeometry(RE::FormID actorID, RE::BSGeometry* geometry) {
        if (!geometry) return false;
        const auto found = layers.find(actorID);
        if (found == layers.end()) return false;
        const auto current = Identity(geometry, Material(geometry));
        return std::ranges::any_of(found->second, [&](const auto& layer) {
            return layer.base && Rendering::OwnsInstalledLayer(layer.identity, current);
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
            retired = bool(layer.base);
        }
        ForgetInstalledGeometry(actorID, geometry);
        return retired;
    }
    void RestoreDisabledDebugLayers() {
        bool hasDebug = false;
        for (const auto& [id, records] : layers) {
            if (std::ranges::any_of(records, [](const auto& layer) { return bool(layer.debugDiffuse); })) {
                hasDebug = true;
                break;
            }
        }
        if (!hasDebug) return;
        auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
        if (!renderer) return;
        renderer->Lock();
        struct Unlock { RE::BSGraphics::Renderer* renderer; ~Unlock() { renderer->Unlock(); } } unlock{renderer};
        for (auto it = layers.begin(); it != layers.end();) {
            std::erase_if(it->second, [&](auto& layer) {
                if (!layer.debugDiffuse) return false;
                RestoreDebugDiffuse(layer, layer.geometry.get(), Material(layer.geometry.get()));
                layer.debugDiffuse.reset();
                layer.originalDiffuse.reset();
                layer.diffuseSource.clear();
                return !layer.base;
            });
            if (it->second.empty()) it = layers.erase(it);
            else ++it;
        }
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
            const bool ownsDebug = OwnsDebug(layer, geometry, source) &&
                replacement->diffuseTexture == source->diffuseTexture &&
                layer.diffuseSource == DiffuseSource(replacement);
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
        RE::BSLightingShaderMaterialBase* material, RE::NiSourceTexture* installed, RE::NiSourceTexture* base, RE::NiSourceTexture* debugDiffuse, RE::NiSourceTexture* originalDiffuse) {
        if (!actor || !geometry || !material || material->normalTexture.get() != installed ||
            !installed || !installed->rendererTexture || !installed->rendererTexture->resourceView ||
            (!base && !debugDiffuse)) return;
        if (base && (!base->rendererTexture || !base->rendererTexture->resourceView ||
            installed->rendererTexture->resourceView == base->rendererTexture->resourceView)) return;
        if (debugDiffuse && (!debugDiffuse->rendererTexture || !debugDiffuse->rendererTexture->resourceView ||
            debugDiffuse != material->diffuseTexture.get())) return;
        try {
            InstalledLayer layer;
            layer.geometry.reset(geometry);
            layer.material.reset(material);
            layer.installed.reset(installed);
            layer.base.reset(base);
            layer.debugDiffuse.reset(debugDiffuse);
            if (debugDiffuse) {
                layer.originalDiffuse.reset(originalDiffuse);
                layer.diffuseSource = DiffuseSource(material);
            }
            layer.textureSet = material->textureSet;
            layer.view = reinterpret_cast<ID3D11ShaderResourceView*>((debugDiffuse ? debugDiffuse : installed)->rendererTexture->resourceView);
            layer.identity = Identity(geometry, material);
            auto& actorLayers = layers[actor->GetFormID()];
            const auto old = std::ranges::find_if(actorLayers, [&](const auto& previous) {
                return previous.geometry.get() == geometry;
            });
            if (old != actorLayers.end()) *old = std::move(layer);
            else actorLayers.push_back(std::move(layer));
        } catch (...) {
            // If bookkeeping cannot grow, do not leave an untracked override.
            if (debugDiffuse && material->diffuseTexture.get() == debugDiffuse)
                material->diffuseTexture.reset(originalDiffuse);
            if (base && material->normalTexture.get() == installed)
                material->normalTexture.reset(base);
            throw;
        }
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
            if (!layer.base) return false; // Ordinary Mu debug survives pregnancy-only restoration.
            auto* material = Material(layer.geometry.get());
            RestoreDebugDiffuse(layer, layer.geometry.get(), material);
            if (!Rendering::OwnsInstalledLayer(layer.identity, Identity(layer.geometry.get(), material))) return true;
            auto base = ResolveRestoredBase(layer.base.get());
            if (!base) return false;
            material->normalTexture = std::move(base);
            return true;
        });
        const bool restored = !std::ranges::any_of(found->second, [](const auto& layer) { return bool(layer.base); });
        if (found->second.empty()) layers.erase(found);
        return restored;
    }
}
