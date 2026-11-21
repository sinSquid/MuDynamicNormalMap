#include "PapyrusScripts.h"

namespace Mus {
    namespace Papyrus {
		std::uint32_t GetVersion(RE::StaticFunctionTag*)
		{
			auto* plugin = SKSE::PluginDeclaration::GetSingleton();
			auto version = plugin->GetVersion();
			std::string sVersion = std::to_string(static_cast<std::uint32_t>(version.major()));
			sVersion += std::to_string(static_cast<std::uint32_t>(version.minor()));
			sVersion += std::to_string(static_cast<std::uint32_t>(version.patch()));
			std::uint32_t result = 0;
			try {
				result = std::stoul(sVersion);
			}
			catch (...) {
				result = 0;
			}
			return result;
		}

		std::uint32_t GetArmorSlotBit(RE::StaticFunctionTag*, std::uint32_t slot)
		{
			if (slot >= 30)
				slot -= 30;
			return slot < 32 ? (1u << slot) : 0u;
		}

		void QUpdateNormalmap(RE::StaticFunctionTag*, RE::Actor* a_actor, std::uint32_t bipedSlot)
		{
			TaskManager::GetSingleton().QUpdateNormalMap(a_actor, bipedSlot);
		}

		namespace {
            std::shared_mutex overrideMutex;
            std::unordered_map<RE::FormID, float> strengths;
            std::unordered_map<RE::FormID, std::string> paths[normalmapTypes::max];
        }

        void ClearActorOverrides() {
            std::unique_lock guard(overrideMutex);
            strengths.clear();
            for (auto& byActor : paths) byActor.clear();
        }

        void StoreDetailStrength(RE::FormID id, float strength) {
            if (!std::isfinite(strength)) return;
            std::unique_lock guard(overrideMutex);
            strengths[id] = std::clamp(strength, 0.0f, 1.0f);
        }
        float ReadDetailStrength(RE::FormID id, float fallback) {
            std::shared_lock guard(overrideMutex);
            const auto it = strengths.find(id);
            return it == strengths.end() ? fallback : it->second;
        }
        void StoreNormalMap(RE::FormID id, normalmapTypes type, std::string path) {
            std::unique_lock guard(overrideMutex);
            paths[type][id] = std::move(path);
        }
        std::string ReadNormalMap(RE::FormID id, normalmapTypes type) {
            std::shared_lock guard(overrideMutex);
            const auto it = paths[type].find(id);
            return it == paths[type].end() ? std::string{} : it->second;
        }
		void SetDetailStrength(RE::StaticFunctionTag*, RE::Actor* a_actor, float strength)
		{
			if (!a_actor)
				return;
			StoreDetailStrength(a_actor->formID, strength);
		}

		int SetNormalMap(RE::StaticFunctionTag*, RE::Actor* a_actor, std::string filePath, int type)
		{
			if (!a_actor || filePath.empty() || type < 0 || type >= normalmapTypes::max)
				return 0;
			if (!IsExistFile(filePath))
				return -1;
			StoreNormalMap(a_actor->formID, static_cast<normalmapTypes>(type), std::move(filePath));
			return 1;
		}

        bool RegisterPapyrusFunctions(RE::BSScript::IVirtualMachine* vm) {
            vm->RegisterFunction("GetVersion", ScriptFileName, GetVersion);
            vm->RegisterFunction("GetArmorSlotBit", ScriptFileName, GetArmorSlotBit);
            vm->RegisterFunction("QUpdateNormalmap", ScriptFileName, QUpdateNormalmap);
            vm->RegisterFunction("SetDetailStrength", ScriptFileName, SetDetailStrength);
            vm->RegisterFunction("SetNormalMap", ScriptFileName, SetNormalMap);
            
            return true;
        }
    }
}
