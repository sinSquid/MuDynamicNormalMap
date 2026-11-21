#pragma once

namespace Mus {
    namespace Papyrus {
        constexpr std::string_view ScriptFileName = "MuDynamicNormalMap";
        bool RegisterPapyrusFunctions(RE::BSScript::IVirtualMachine* vm);
        void ClearActorOverrides();
		void StoreDetailStrength(RE::FormID id, float strength);
        float ReadDetailStrength(RE::FormID id, float fallback);
        enum normalmapTypes {
            detail,
            overlay,
            mask,
            max
        };
        void StoreNormalMap(RE::FormID id, normalmapTypes type, std::string path);
        std::string ReadNormalMap(RE::FormID id, normalmapTypes type);
    }
}
