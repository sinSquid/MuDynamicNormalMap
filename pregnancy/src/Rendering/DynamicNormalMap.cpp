// SPDX-License-Identifier: GPL-3.0-only
#include "pch.h"
#include "Rendering/DynamicNormalMap.h"
#include "Core/Config.h"
#include "Core/TextureCache.h"
#include <DirectXPackedVector.h>
#include "Actor/ActorManager.h"
#include "Rendering/MuBridge.h"
#include "Compat/OSkin.h"
#include "Lifecycle.h"

namespace PNS {
    namespace {
        using namespace Rendering;
        float Half(const std::uint8_t* data) {
            std::uint16_t value; std::memcpy(&value, data, sizeof(value));
            return DirectX::PackedVector::XMConvertHalfToFloat(value);
        }
        std::vector<std::uint8_t> MeshMask(RE::BSGeometry* geometry, const PregnancyRegion& region) {
            auto* shape = geometry->AsTriShape();
            if (!shape) return {};
            auto& runtime = geometry->GetGeometryRuntimeData();
            auto extract = [&](RE::BSGraphics::TriShape* data, RE::BSGraphics::VertexDesc desc, unsigned count, std::span<const std::uint16_t> indices) {
                if (!data || !data->rawVertexData || count == 0 || count > 65535 || indices.empty()) return std::vector<std::uint8_t>{};
                if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_UV)) return std::vector<std::uint8_t>{};
                std::uint64_t bits;
                std::memcpy(&bits, &desc, sizeof(bits));
                const unsigned stride = static_cast<unsigned>(bits & 15) * 4;
                const auto uvOffset = desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
                const bool full = uvOffset >= 16 || desc.HasFlag(RE::BSGraphics::Vertex::VF_FULLPREC);
                if (uvOffset < (full ? 12u : 8u) || uvOffset + 4 > stride) return std::vector<std::uint8_t>{};
                std::vector<MaskVertex> vertices; vertices.reserve(count);
                for (unsigned i=0; i<count; ++i) {
                    const auto* v = data->rawVertexData + size_t(i)*stride;
                    MaskVertex vertex{};
                    if (full) std::memcpy(&vertex.x,v,3*sizeof(float));
                    else { vertex.x = Half(v); vertex.y = Half(v+2); vertex.z = Half(v+4); }
                    vertex.u = Half(v+uvOffset); vertex.v = Half(v+uvOffset+2);
                    vertices.push_back(vertex);
                }
                return RasterizePregnancyMask(vertices,indices,region);
            };
            const auto& counts = shape->GetTrishapeRuntimeData();
            if (auto* skin = runtime.skinInstance.get(); skin && skin->skinPartition) {
                auto* partitions = skin->skinPartition.get();
                if (partitions->numPartitions && partitions->partitions.data()) {
                    std::vector<std::uint16_t> indices;
                    for (unsigned i = 0; i < partitions->numPartitions; ++i) {
                        const auto& part = partitions->partitions[i];
                        if (part.triList && part.triangles)
                            indices.insert(indices.end(), part.triList, part.triList + size_t(part.triangles)*3);
                    }
                    const auto& first = partitions->partitions[0];
                    auto mask = extract(first.buffData, first.vertexDesc, partitions->vertexCount, indices);
                    if (!mask.empty()) return mask;
                }
            }
            auto* data = runtime.rendererData;
            if (!data || !data->rawIndexData) return {};
            return extract(data, runtime.vertexDesc, counts.vertexCount, {data->rawIndexData, size_t(counts.triangleCount)*3});
        }

        RE::NiPointer<RE::NiSourceTexture> LoadNormal(const std::string& path) {
            if (path.empty()) return {};
            auto normalized = path;
            std::replace(normalized.begin(),normalized.end(),'/','\\');
            RE::NiPointer<RE::BSShaderTextureSet> set{RE::BSShaderTextureSet::Create()};
            if (!set) return {};
            set->SetTexturePath(RE::BSTextureSet::Texture::kNormal,normalized.c_str());
            RE::NiPointer<RE::NiSourceTexture> result;
            set->SetTexture(RE::BSTextureSet::Texture::kNormal,result); return result;
        }
        RE::NiPointer<RE::NiSourceTexture> LoadTarget(RE::Actor* actor, const BodyProfile& profile) {
            auto& cache = TextureCache::GetSingleton();
            const auto* base = actor->GetActorBase();
            if (!base) return {};
            const auto* race = actor->GetRace();
            auto directory = cache.ResolveRaceDir(race ? race->GetFormEditorID() : "");
            auto path = profile.pregnancyNormal.empty() ? cache.GetPregnancyTarget(directory,base->GetSex()==RE::SEX::kFemale,base->GetWeight()) : profile.pregnancyNormal;
            return LoadNormal(path);
        }
        class RendererLock {
        public:
            RendererLock() : renderer_(RE::BSGraphics::Renderer::GetSingleton()) { renderer_->Lock(); }
            ~RendererLock() { renderer_->Unlock(); }
        private: RE::BSGraphics::Renderer* renderer_;
        };
    }


    RE::BSLightingShaderMaterialBase* PrivateNormalMaterial(RE::FormID actorID, RE::BSGeometry* geometry) {
        if (!geometry || !ModuleEnabled() || !SessionActive()) return nullptr;
        RendererLock lock;
        auto& runtime = geometry->GetGeometryRuntimeData();
        auto* source = netimmerse_cast<RE::BSLightingShaderProperty*>(runtime.shaderProperty.get());
        if (!source || !source->material) return nullptr;
        // Keep the old material alive until OSkin has checked its source identity.
        RE::NiPointer<RE::BSLightingShaderProperty> original{source};
        RE::NiPointer<RE::NiObject> copy{source->Clone()};
        auto* property = netimmerse_cast<RE::BSLightingShaderProperty*>(copy.get());
        if (!property || property == source) return nullptr;
        RE::BSTSmartPointer<RE::BSShaderMaterial> material{source->material->Create()};
        if (!material) return nullptr;
        material->CopyMembers(source->material);
        auto* lighting = skyrim_cast<RE::BSLightingShaderMaterialBase*>(material.get());
        if (!lighting) return nullptr;
        property->SetMaterial(material.get(), true);
        runtime.shaderProperty.reset(property);
        auto* installed = skyrim_cast<RE::BSLightingShaderMaterialBase*>(property->material);
        OSkin::OnMaterialCloned(actorID, geometry,
            skyrim_cast<RE::BSLightingShaderMaterialBase*>(original->material), installed);
        OnInstalledMaterialCloned(actorID, geometry,
            skyrim_cast<RE::BSLightingShaderMaterialBase*>(original->material), installed);
        return installed;
    }

    bool ReadAssignedNormal(const std::string& path, ComPtr<ID3D11Texture2D>& texture,
        ComPtr<ID3D11ShaderResourceView>& view) {
        if (!ModuleEnabled() || !SessionActive()) return false;
        auto skin = LoadNormal(path);
        if (!skin || !skin->rendererTexture || !skin->rendererTexture->resourceView) return false;
        auto* assigned = reinterpret_cast<ID3D11ShaderResourceView*>(skin->rendererTexture->resourceView);
        ComPtr<ID3D11Resource> resource;
        assigned->GetResource(resource.GetAddressOf());
        ComPtr<ID3D11Texture2D> input;
        if (!resource || FAILED(resource.As(&input))) return false;
        texture = std::move(input);
        view = assigned;
        return true;
    }

    void ComposeMuBody(RE::Actor* actor, std::span<RE::BSGeometry* const> identities,
        ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>& view, std::string_view sourcePath, bool dinoBase) {
        if (!ModuleEnabled() || !SessionActive() || !actor) return;
        const auto [amount, stage] = ActorManager::GetSingleton().GetPregnancyLayer(actor->GetFormID());
        if (amount <= 0 || !texture || !view || identities.empty()) return;
        D3D11_SHADER_RESOURCE_VIEW_DESC inputDesc{};
        view->GetDesc(&inputDesc);
        if (inputDesc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
            inputDesc.Format == DXGI_FORMAT_BC5_UNORM || inputDesc.Format == DXGI_FORMAT_BC5_SNORM) return;
        RendererLock lock;
        const auto& config = Config::GetSingleton().GetDynamicTextures();
        auto* root = actor->Get3D();
        if (!root) return;
        // A job can finish after an equipment change. Only inspect a still-attached shape.
        const BodyProfile* profile = nullptr;
        bool tangent = false;
        std::vector<std::uint8_t> mask;
        std::function<void(RE::NiAVObject*)> visit = [&](RE::NiAVObject* object) {
            if (!object) return;
            auto* geometry = object->AsGeometry();
            if (geometry && std::ranges::find(identities, geometry) != identities.end()) {
                for (const auto& entry : config.profiles) {
                    if (config.bodyProfile != "Auto" && config.bodyProfile != entry.name) continue;
                    if (std::ranges::find(entry.nodes, object->name.c_str()) != entry.nodes.end()) {
                        if (profile && profile != &entry) continue;
                        auto part = MeshMask(geometry, entry.belly);
                        if (part.empty()) break;
                        profile = &entry;
                        auto* shader = netimmerse_cast<RE::BSLightingShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get());
                        tangent = dinoBase && shader && !shader->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals);
                        if (mask.empty()) mask = std::move(part);
                        else for (size_t i = 0; i < mask.size(); ++i) mask[i] = std::max(mask[i], part[i]);
                        break;
                    }
                }
            } else if (auto* node = object->AsNode()) {
                for (const auto& child : node->GetChildren()) visit(child.get());
            }
        };
        visit(root);
        if (!profile) return;


        auto compatibleView = [&](RE::NiSourceTexture* src) -> ID3D11ShaderResourceView* {
            if (!src || !src->rendererTexture) return nullptr;
            auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>(src->rendererTexture->resourceView);
            if (!srv) return nullptr;
            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
            srv->GetDesc(&desc);
            ComPtr<ID3D11Device> expected, actual;
            view->GetDevice(&expected);
            srv->GetDevice(&actual);
            if (expected.Get() != actual.Get() || desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
                (!tangent && (desc.Format == DXGI_FORMAT_BC5_UNORM || desc.Format == DXGI_FORMAT_BC5_SNORM))) return nullptr;
            return srv;
        };
        RE::NiPointer<RE::NiSourceTexture> skin, target;
        const bool actorSkin = dinoBase || OSkin::Active();
        if (actorSkin)
            skin = LoadNormal(std::string(sourcePath));
        else if (!config.useActorSkin)
            skin = LoadNormal(profile->baseNormal.empty() ? config.baseNormal : profile->baseNormal);
        if (!stage.empty()) target = LoadNormal(stage);
        else if (config.usePregnancyTarget && (!actorSkin || !profile->pregnancyNormal.empty()))
            target = LoadTarget(actor, *profile);
        auto* detail = compatibleView(skin.get());
        if (actorSkin && !detail) return;
        auto* endpoint = compatibleView(target.get());
        ComPtr<ID3D11Device> device;
        texture->GetDevice(&device);
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        const float scale = config.maxResolution && !dinoBase ? std::min(1.0f, float(config.maxResolution) /
            std::max(desc.Width, desc.Height)) : 1.0f;
        NormalBlend blend;
        if (!blend.Initialize(device.Get()) || !blend.CreateOutput(std::max(1u, unsigned(scale*desc.Width)), std::max(1u, unsigned(scale*desc.Height))) ||
            !blend.SetMask(mask, 256) || !blend.Render(context.Get(), view.Get(), endpoint,
            {amount, config.smoothing, config.blurRadius, endpoint != nullptr, dinoBase}, detail)) {
            logger::warn("Pregnancy composition failed on {:08X}: {}", actor->GetFormID(), blend.Error());
            return;
        }
        texture = blend.Texture();
        view = blend.View();
    }
}
