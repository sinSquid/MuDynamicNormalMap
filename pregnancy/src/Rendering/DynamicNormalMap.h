#pragma once
#include "Rendering/NormalBlend.h"

namespace PNS {
    struct BodyProfile {
        std::string name;
        std::vector<std::string> nodes;
        Rendering::PregnancyRegion belly;
        std::string pregnancyNormal; // Optional texture path relative to Data, including textures/.
        std::string baseNormal;
    };
    struct DynamicTextureConfig {
        bool enabled = true;
        std::string bodyProfile = "Auto";
        bool usePregnancyTarget = true;
        bool useActorSkin = false;
        std::string baseNormal = "textures/actors/character/female/femalebody_1_msn.dds";
        float smoothing = 0.85f;
        float blurRadius = 0.008f;
        unsigned maxResolution = 0;
        std::vector<BodyProfile> profiles;
    };

}
