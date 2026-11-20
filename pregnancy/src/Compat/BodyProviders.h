#pragma once
namespace PNS::BodyProviders {
    void Initialize();
    void Observe(RE::Actor* actor);
    void Request(RE::Actor* actor, std::uint32_t slots);
    void Tick();
    void Forget(RE::FormID actor);
    void Clear();
}

