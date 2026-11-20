#include "pch.h"
#include "Compat/BodyProviders.h"
#include "Compat/OSkin.h"
#include "Compat/ProviderMorphs.h"
#include "Compat/SkinPaths.h"

namespace PNS::OSkin {
    namespace {
        bool enabled = false;
        // OSkin swaps the whole material even for a diffuse-only assignment. Mu changes
        // the normal texture inside the material, so its own writes do not change this key.
        using Material = std::tuple<std::uintptr_t, std::uintptr_t, std::string>;
        std::unordered_map<RE::FormID, std::vector<Material>> assignments;
    }
    void Initialize() {
        enabled = GetModuleHandleW(L"OSkin.dll") != nullptr;
        if (enabled) logger::info("OSkin material tracking enabled");
    }
    bool Active() { return enabled; }
    void Forget(RE::FormID id) { assignments.erase(id); }
    void Clear() { assignments.clear(); }

    void Observe(RE::Actor* actor) {
        if (!enabled || !actor || !actor->Get3D()) return;
        std::vector<Material> current;
        using Slot = RE::BSTextureSet::Texture;
        RE::BSVisit::TraverseScenegraphGeometries(actor->Get3D(), [&](RE::BSGeometry* g) {
            auto* property = netimmerse_cast<RE::BSLightingShaderProperty*>(g->GetGeometryRuntimeData().shaderProperty.get());
            auto* mat = property ? skyrim_cast<RE::BSLightingShaderMaterialBase*>(property->material) : nullptr;
            if (mat && mat->textureSet && property->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals)) {
                for (unsigned slot = 0; slot < Slot::kTotal; ++slot) {
                    const char* path = mat->textureSet->GetTexturePath(static_cast<Slot>(slot));
                    if (!path || !SkinPaths::IsPackPath(path)) continue;
                    const char* normal = mat->textureSet->GetTexturePath(Slot::kNormal);
                    current.emplace_back(reinterpret_cast<std::uintptr_t>(g), reinterpret_cast<std::uintptr_t>(mat),
                        normal ? SkinPaths::Relative(normal) : std::string{});
                    break;
                }
            }
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        const auto id = actor->GetFormID();
        if (current.empty()) {
            if (!assignments.erase(id)) return;
        } else {
            std::sort(current.begin(), current.end());
            auto& previous = assignments[id];
            if (previous == current) return;
            previous = std::move(current);
        }
        BodyProviders::Request(actor, ProviderMorphs::allSlots);
    }
}

