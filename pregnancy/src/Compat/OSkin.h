#pragma once
namespace PNS::OSkin {
    void Initialize();
    void Observe(RE::Actor* actor);
    void Forget(RE::FormID id);
    void Clear();
    bool Active();
}

