#pragma once
#include <cstdint>
#include <string>

namespace PNS::Rendering {
    struct InstalledLayerIdentity {
        std::uintptr_t geometry = 0, material = 0, texture = 0, view = 0, textureSet = 0;
        std::string normalPath;
        bool operator==(const InstalledLayerIdentity&) const = default;
    };
    inline bool OwnsInstalledLayer(const InstalledLayerIdentity& installed,
        const InstalledLayerIdentity& current) {
        return installed.geometry && installed.material && installed.texture && installed.view &&
            installed == current;
    }
    inline bool OwnsDebugDiffuse(const InstalledLayerIdentity& installed,
        const InstalledLayerIdentity& current, std::uintptr_t diffuse, std::uintptr_t diffuseView) {
        return installed.geometry && installed.material && installed.texture && installed.view &&
            installed.geometry == current.geometry && installed.material == current.material &&
            installed.textureSet == current.textureSet && installed.normalPath == current.normalPath &&
            installed.texture == diffuse && installed.view == diffuseView;
    }
}
