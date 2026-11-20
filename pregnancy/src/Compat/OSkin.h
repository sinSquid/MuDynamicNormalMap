#pragma once
namespace PNS::OSkin {
    void Initialize();
    void Observe(RE::Actor* actor);
    // Game-thread notification; both materials remain alive during this call.
    void OnMaterialCloned(RE::FormID actorID, RE::BSGeometry* geometry, RE::BSLightingShaderMaterialBase* source,
        RE::BSLightingShaderMaterialBase* replacement);
    void Forget(RE::FormID id);
    void Clear();
    bool Active();
}

