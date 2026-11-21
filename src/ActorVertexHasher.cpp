#include "ActorVertexHasher.h"

namespace Mus {
    ActorVertexHasher::~ActorVertexHasher() {
        WaitForPendingDetection();
    }

    void ActorVertexHasher::WaitForPendingDetection() {
        // The callback takes actor locks itself; never hold them while waiting.
        if (pendingDetection.valid())
            pendingDetection.wait();
    }

    void ActorVertexHasher::onEvent(const QuitGameEvent& e) {
        detectionStopped.store(true);
        WaitForPendingDetection();
    }

	void ActorVertexHasher::Init()
	{

	}

    void ActorVertexHasher::ResetRegistrations() {
        { std::lock_guard lock(actorHashLock); actorHash.clear(); foregroundCycle.Reset(); }
        { std::lock_guard lock(blockActorsLock); blockActors.clear(); }
    }

	void ActorVertexHasher::onEvent(const FrameEvent& e)
	{
        const bool loading = IsSaveLoading.load() || IsInLoading.load();
        if (loading) foregroundCycle.Reset();
		if (detectionStopped.load() || tuningPaused.load())
            return;
        if (!Config::GetSingleton().GetRealtimeDetect()) {
            foregroundCycle.Reset();
            return;
        }
		static std::clock_t lastTickTime = currentTime;

		if (loading)
			return;
		if (IsGamePaused.load() && !IsRaceSexMenu.load())
			return;
		if (detectionGate.Busy())
			return;
        if (Config::GetSingleton().GetRealtimeDetectOnBackGround()) foregroundCycle.Reset();
        const bool startingPass = !foregroundCycle.Active();
		if (startingPass && lastTickTime + Config::GetSingleton().GetDetectTickMS() > currentTime)
			return;
		CheckingActorHash();
        // Continue a partial foreground pass on consecutive frames, while the
        // configured period remains start-to-start as with background detection.
		if (startingPass) lastTickTime = currentTime;
	}

	void ActorVertexHasher::onEvent(const FacegenNiNodeEvent& e)
	{
		if (!e.facegenNiNode)
			return;
		RE::Actor* actor = skyrim_cast<RE::Actor*>(e.facegenNiNode->GetUserData());
		if (!actor)
			return;
		SetBlocked(actor->formID, true);
	}

	void ActorVertexHasher::onEvent(const ActorChangeHeadPartEvent& e)
	{
		if (!e.actor)
			return;
		SetBlocked(e.actor->formID, true);
	}

	void ActorVertexHasher::onEvent(const ArmorAttachEvent& e)
	{
		if (e.hasAttached)
			return;
		if (!e.actor)
			return;
		SetBlocked(e.actor->formID, true);
	}

	EventResult ActorVertexHasher::ProcessEvent(const SKSE::NiNodeUpdateEvent* evn, RE::BSTEventSource<SKSE::NiNodeUpdateEvent>*)
	{
		if (!evn || !evn->reference)
			return EventResult::kContinue;
		SetBlocked(evn->reference->formID, true);
		return EventResult::kContinue;
	}

    bool ActorVertexHasher::CanDetectGeometry(RE::BSGeometry* geometry) {
        if (!geometry || !geometry->AsTriShape()) return false;
        const auto desc = geometry->GetGeometryRuntimeData().vertexDesc;
        if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_UV)) return false;
        return !geometry->AsDynamicTriShape() || Config::GetSingleton().GetRealtimeDetectHead() != 0;
    }

	bool ActorVertexHasher::Register(RE::Actor* a_actor, RE::BSGeometry* a_geo)
	{
		if (!Config::GetSingleton().GetRealtimeDetect())
			return true;
		if (!a_geo || a_geo->name.empty())
			return false;
        if (!CanDetectGeometry(a_geo)) return false;
		if (IsInvalidActor(a_actor))
			return false;
        std::lock_guard lg(actorHashLock);
        auto actorIt = actorHash.try_emplace(a_actor->formID).first;
        auto hashIt = actorIt->second.hash.find(a_geo);
        if (hashIt != actorIt->second.hash.end())
        {
            hashIt->second->SetCheckTexture(false);
            return hashIt->second->Update(a_geo);
		}
		else
		{
            auto newHash = std::make_shared<Hash>(a_geo);
            newHash->SetCheckTexture(false);
            newHash->AcceptBaseline();
            actorIt->second.hash.insert(std::make_pair(a_geo, newHash));
            logger::debug("{:x} {} {}: registered for ActorVertexHasher", a_actor->formID, a_actor->GetName(), a_geo->name.c_str());
		}
        return true;
    }
	bool ActorVertexHasher::RegisterCheckTexture(RE::Actor* a_actor, RE::BSGeometry* a_geo)
	{
        if (!Config::GetSingleton().GetDetectTextureChange())
            return true;
        std::lock_guard lg(actorHashLock);
        auto actorIt = actorHash.find(a_actor->formID);
        if (actorIt == actorHash.end())
            return false;
        auto hashIt = actorIt->second.hash.find(a_geo);
        if (hashIt == actorIt->second.hash.end())
            return false;
        hashIt->second->SetCheckTexture(true);
        return true;
	}

	void ActorVertexHasher::CheckingActorHash()
	{
        {
            std::shared_lock lock(actorHashLock);
            if (actorHash.empty()) {
                foregroundCycle.Reset();
                return;
            }
        }
		auto camera = RE::PlayerCamera::GetSingleton();
		if (!camera)
			return;

		const std::string funcName = __func__;
		auto cameraPosition = camera->GetRuntimeData2().pos;

        const auto tuning = Config::GetSingleton().GetRuntimeTuning();
        const float detectDistanceSquared = tuning->detectDistance * tuning->detectDistance;
		auto actorHashFunc = [&, funcName, cameraPosition, tuning, detectDistanceSquared](const RE::FormID& id, GeometryHashData& data, std::size_t* hashes = nullptr) {
			if (data.hash.empty()) return false;
            RE::NiPointer<RE::Actor> actor = RE::NiPointer(GetFormByID<RE::Actor*>(id));
            if (IsInvalidActor(actor.get()))
			{
				return false;
			}
			if (Config::GetSingleton().GetActorVertexHasherTime2())
				PerformanceLog(funcName + "::" + GetHexStr(id), false, true);

			if (IsBlocked(actor->formID))
				return true;
			if (!IsPlayer(actor->formID) && (detectDistanceSquared > floatPrecision && cameraPosition.GetSquaredDistance(actor->loadedData->data3D->world.translate) > detectDistanceSquared))
                return true;
            if (hashes) ++*hashes;
            if (!GetHash(actor.get(), data)) return true;
            if (data.hash.empty()) return false;
            if (data.refreshPending) {
                TaskManager::GetSingleton().RegisterDelayTask([id]() {
                    auto actor = GetFormByID<RE::Actor*>(id);
                    if (IsInvalidActor(actor))
                        return;
                    TaskManager::GetSingleton().QUpdateNormalMap(actor);
                });
                data.refreshPending = false;
			}

			if (Config::GetSingleton().GetActorVertexHasherTime2())
				PerformanceLog(funcName + "::" + GetHexStr(id), true, true);
            return true;
		};

		if (Config::GetSingleton().GetRealtimeDetectOnBackGround())
		{
            auto ticket = detectionGate.TryAcquire();
            if (!ticket)
                return;
			if (Config::GetSingleton().GetActorVertexHasherTime1())
				PerformanceLog(funcName, false, true);

			{
                std::lock_guard lg(blockActorsLock);
                blockActors.clear();
            }

			pendingDetection = backGroundWorkerThreads->submitAsync([this, actorHashFunc, funcName, ticket = std::move(ticket)]() mutable {
                // A retained future owns its packaged callable. Release the ticket
                // on invocation exit instead of waiting for that callable to die.
                auto activeTicket = std::move(ticket);
				{
                    std::lock_guard lg(actorHashLock);
                    for (auto it = actorHash.begin(); it != actorHash.end();)
                    {
						if (!actorHashFunc(it->first, it->second))
						{
                            it = actorHash.erase(it);
						}
						else
                        {
                            it++;
						}
                    }
                    if (Config::GetSingleton().GetActorVertexHasherTime1())
                        PerformanceLog(funcName, true, true, actorHash.size());
                    logger::trace("Check hashes done {}", actorHash.size());
                }
			});
		}
		else
		{
			if (Config::GetSingleton().GetActorVertexHasherTime1())
				PerformanceLog(std::string(__func__), false, true);

            // Retain blocks throughout a split pass, just like one background pass.
            if (!foregroundCycle.Active()) {
                std::lock_guard lg(blockActorsLock);
                blockActors.clear();
            }
            std::lock_guard lg(actorHashLock);
            if (!foregroundCycle.Active()) foregroundCycle.Begin(actorHash);
            const auto started = std::chrono::steady_clock::now();
            std::size_t hashes = 0, inspected = 0;
            while (foregroundCycle.Active()) {
                if (hashes >= 4 || inspected >= 32 ||
                    (inspected && std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(2))) break;
                auto it = foregroundCycle.TakeNext(actorHash);
                if (it == actorHash.end()) break;
                ++inspected;
                if (!actorHashFunc(it->first, it->second, &hashes)) actorHash.erase(it);
            }
            logger::trace("Check hashes slice: {} inspected, {} hashed, remaining={}", inspected, hashes, foregroundCycle.Active());

			if (Config::GetSingleton().GetActorVertexHasherTime1())
                PerformanceLog(std::string(__func__), true, true, hashes);
		}
	}

	bool ActorVertexHasher::GetHash(RE::Actor* a_actor, GeometryHashData& scan)
	{
		if (IsInvalidActor(a_actor))
			return false;
		if (IsBlocked(a_actor->formID))
            return false;

		if (Config::GetSingleton().GetActorVertexHasherTime2())
			PerformanceLog(std::string(__func__), false, false);

		auto& data = scan.hash;
        bool interrupted = false;
        std::unordered_set<RE::BSGeometry*> existGeometries;
		auto root = a_actor->loadedData->data3D;
		RE::BSVisit::TraverseScenegraphGeometries(root.get(), [&](RE::BSGeometry* geo) -> RE::BSVisit::BSVisitControl {
			using Feature = RE::BSShaderMaterial::Feature;
			if (IsBlocked(a_actor->formID)) {
                interrupted = true;
                return RE::BSVisit::BSVisitControl::kStop;
            }
            if (!geo || geo->name.empty())
				return RE::BSVisit::BSVisitControl::kContinue;
            auto found = data.find(geo);
			if (found == data.end())
				return RE::BSVisit::BSVisitControl::kContinue;
            if (!CanDetectGeometry(geo)) return RE::BSVisit::BSVisitControl::kContinue;
            // A live shape whose CPU buffers are temporarily unavailable must
            // remain registered so the next detection pass can retry it.
            existGeometries.insert(geo);
			if (found->second->Update(geo)) {
                auto& hash = *found->second;
                if (hash.GetHash() != hash.GetOldHash()) scan.refreshPending = true;
                if (hash.IsChangedTexture()) {
                    hash.SetCheckTexture(false);
                    scan.refreshPending = true;
                }
            }
			return RE::BSVisit::BSVisitControl::kContinue;
		});

		// Only a complete traversal can establish that a registered shape disappeared.
        if (interrupted || IsBlocked(a_actor->formID)) return false;
        for (auto it = data.begin(); it != data.end();)
		{
			if (existGeometries.find(it->first) == existGeometries.end())
				it = data.erase(it);
			else
				it++;
        }

		if (Config::GetSingleton().GetActorVertexHasherTime2())
			PerformanceLog(std::string(__func__), true, false);

		return true;
	}

	bool ActorVertexHasher::Hash::Update(RE::BSGeometry* a_geo)
    {
        if (!a_geo)
            return false;
        const RE::BSTriShape* triShape = a_geo->AsTriShape();
        if (!triShape)
            return false;
        std::uint32_t vertexCount = triShape->GetTrishapeRuntimeData().vertexCount;
        const RE::NiPointer<RE::NiSkinPartition> skinPartition = GetSkinPartition(a_geo);
        if (!skinPartition || !skinPartition->numPartitions || skinPartition->partitions.empty())
            return false;
        vertexCount = vertexCount > 0 ? vertexCount : skinPartition->vertexCount;
        const auto* buffer = skinPartition->partitions[0].buffData;
        if (!vertexCount || !buffer || !buffer->rawVertexData) return false;
        auto desc = a_geo->GetGeometryRuntimeData().vertexDesc;
        if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) || !desc.HasFlag(RE::BSGraphics::Vertex::VF_UV) || !desc.GetSize())
            return false;

        Reset();
        if (const RE::BSDynamicTriShape* dynamicTriShape = a_geo->AsDynamicTriShape(); dynamicTriShape)
        {
            if (Config::GetSingleton().GetRealtimeDetectHead() == 1)
            {
                const auto morphData = GeometryData::GetMorphExtraData(a_geo);
                if (morphData && morphData->vertexData && vertexCount <= morphData->modelVertexCount)
                {
                    // FOD storage is allocated with modelVertexCount, not the
                    // currently displayed shape's vertex count.
                    Update(morphData->vertexData, sizeof(RE::NiPoint3) * vertexCount);
                }
            }
            else if (Config::GetSingleton().GetRealtimeDetectHead() == 2)
            {
                const auto* dynamicData = dynamicTriShape->GetDynamicTrishapeRuntimeData().dynamicData;
                if (dynamicData) Update(dynamicData, sizeof(DirectX::XMVECTOR) * vertexCount);
            }
            else
                return false;
        }
        Update(buffer->rawVertexData, sizeof(std::uint8_t) * vertexCount * desc.GetSize());
        GetNewHash();
        if (isCheckTexture)
            isCheckTexture = CheckTexture(a_geo);
        return true;
    }

	bool ActorVertexHasher::Hash::CheckTexture(RE::BSGeometry* a_geo)
    {
        isMDNMTexture = false;
        if (!a_geo)
            return false;
        if (!a_geo->GetGeometryRuntimeData().shaderProperty)
            return false;
        auto effect = a_geo->GetGeometryRuntimeData().shaderProperty.get();
        auto lightingShader = netimmerse_cast<RE::BSLightingShaderProperty*>(effect);
        if (!lightingShader || !lightingShader->material)
            return false;
        RE::BSLightingShaderMaterialBase* material = skyrim_cast<RE::BSLightingShaderMaterialBase*>(lightingShader->material);
        if (!material || !material->normalTexture || material->normalTexture->name.empty())
            return false;
        isMDNMTexture = IsCreatedByMDNM(material->normalTexture->name.c_str());
        return true;
    }
}
