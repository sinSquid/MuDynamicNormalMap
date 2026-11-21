// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <wrl/client.h>

// Loaded on kDataLoaded, after both SKSE modules have initialized. The unmodified
// Mu DLL and older Pregnancy Normal Map++ builds keep Dino's original conflict guard.
namespace RMF::PregnancyBridge {
    using Key = std::uint64_t (*)(std::uint32_t);
    using Compose = bool (*)(std::uint32_t, RE::BSGeometry*, const char*, ID3D11Texture2D*,
        ID3D11ShaderResourceView*, ID3D11Texture2D**, ID3D11ShaderResourceView**);
    inline Key key = nullptr;
    inline Compose compose = nullptr;

    inline bool Connect(HMODULE module) {
        key = reinterpret_cast<Key>(GetProcAddress(module, "PNS_DinoLayerKeyV1"));
        compose = reinterpret_cast<Compose>(GetProcAddress(module, "PNS_DinoComposeV1"));
        return key && compose;
    }

    struct Layer {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> bake;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> bakeView;
        std::uint64_t previousKey = 0;
        RE::BSGeometry* previousGeometry = nullptr;
        bool displayed = false;

        void Capture(ID3D11Texture2D* texture, ID3D11ShaderResourceView* view) {
            if (!compose || !key) return;
            bake = texture;
            bakeView = view;
            previousKey = 0;
            previousGeometry = nullptr;
            displayed = false;
        }

        template<class Install>
        void Update(std::uint32_t actor, RE::BSGeometry* geometry,
            const std::string& skin, Install install) {
            if (!bake || !bakeView || !key || !compose) return;
            const auto stamp = key(actor);
            if (previousKey == stamp && previousGeometry == geometry) return;
            previousKey = stamp;
            previousGeometry = geometry;
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
            if (stamp && compose(actor, geometry, skin.c_str(), bake.Get(), bakeView.Get(),
                    texture.GetAddressOf(), view.GetAddressOf())) {
                displayed = true;
            } else {
                // A transient composition failure must not acknowledge this layer.
                // Retry the same nonzero key on the next update, even after reverting.
                if (stamp) { previousKey = 0; previousGeometry = nullptr; }
                if (!displayed) return;
                texture = bake;
                view = bakeView;
                displayed = false;
            }
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            install(texture.Detach(), view.Detach(), desc.Width, desc.MipLevels);
        }
    };
}

