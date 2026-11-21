#pragma once

namespace MDNM {
	class DynamicNormalMap :
		public IDynamicNormalMap
	{
	public:
		virtual std::uint32_t GetVersion() override { return 0; };
		
		virtual void QUpdateNormalmap(RE::Actor* a_actor, std::uint32_t a_updateBipedSlot) override {
			Mus::TaskManager::GetSingleton().QUpdateNormalMap(a_actor, a_updateBipedSlot);
		}
		virtual void SetDetailStrength(RE::Actor* a_actor, float a_strength) {
			if (!a_actor)
				return;
			Mus::Papyrus::StoreDetailStrength(a_actor->formID, a_strength);
		}
		virtual int SetNormalMap(RE::Actor* a_actor, const char* a_filePath, int type) {
			if (!a_filePath)
				return 0;
			std::string filePath = a_filePath;
			if (!a_actor || filePath.empty() || type < 0 || type >= normalmapTypes::max)
				return 0;
			if (!Mus::IsExistFile(filePath))
				return -1;
			Mus::Papyrus::StoreNormalMap(a_actor->formID, static_cast<Mus::Papyrus::normalmapTypes>(type), std::move(filePath));
			return 1;
		}
	};
	static DynamicNormalMap DNM;
}
