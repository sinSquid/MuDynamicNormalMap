#include "TaskManager.h"
#include "SettingsCatalog.h"

namespace Mus {
    bool TaskManager::IsActorEnabled(RE::Actor* actor) const {
        if (!actor) return false;
        const auto tuning = Config::GetSingleton().GetRuntimeTuning();
        if (IsPlayer(actor->formID) ? !tuning->PlayerEnable : !tuning->NPCEnable) return false;
        const auto sex = GetSex(actor);
        return !(sex == RE::SEX::kMale && !tuning->MaleEnable) &&
               !(sex == RE::SEX::kFemale && !tuning->FemaleEnable);
    }
    void TaskManager::QueueMenuApply(const RuntimeTuning& tuning, MenuApplyCompletion completion) {
        if (stoppingComputations.load() || pendingMenuApply) {
            completion(false, "已有配置正在应用，或游戏正在退出。");
            return;
        }
        RuntimeTuning normalized;
        std::string error;
        if (!NormalizeRuntimeTuning(tuning, normalized, error)) {
            completion(false, error);
            return;
        }
        pendingMenuApply = MenuApplyRequest{std::move(normalized), std::move(completion), std::chrono::steady_clock::now()};
        ActorVertexHasher::GetSingleton().PauseForMenuApply(true);
    }

    void TaskManager::FinishMenuApply(bool success, const std::string& status) {
        if (!pendingMenuApply) return;
        auto completion = std::move(pendingMenuApply->completion);
        pendingMenuApply.reset();
        ActorVertexHasher::GetSingleton().PauseForMenuApply(false);
        if (completion) completion(success, status);
    }

    void TaskManager::RefreshRegisteredActors() {
        std::vector<RE::FormID> actors;
        {
            std::shared_lock lock(isActiveActorsLock);
            actors.reserve(isActiveActors.size());
            for (const auto& [id, active] : isActiveActors) actors.push_back(id);
        }
        { std::lock_guard lock(updateSlotQueueLock); updateSlotQueue.clear(); }
        ActorVertexHasher::GetSingleton().ResetRegistrations();
        geometryRetry.Clear();
        for (const auto id : actors) {
            auto actor = GetFormByID<RE::Actor*>(id);
            if (IsInvalidActor(actor)) continue;
            // Restore first so disabled actors and excluded body parts do not retain old maps.
            RemoveNormalMap(actor);
            QUpdateNormalMap(actor, BipedObjectSlot::kAll);
        }
    }

    void TaskManager::ProcessMenuApply() {
        if (!pendingMenuApply || menuApplyScheduled) return;
        if (std::chrono::steady_clock::now() - pendingMenuApply->queuedAt > std::chrono::seconds(30)) {
            FinishMenuApply(false, "等待当前法线任务结束超时；配置未应用，请稍后重试。");
            return;
        }
        auto& updater = ObjectNormalMapUpdater::GetSingleton();
        if (HasPendingUpdates() || ActorVertexHasher::GetSingleton().HasPendingDetection() ||
            updater.GetCount() > 0 || updater.HasPendingCleanup() ||
            NormalMapStore::GetSingleton().HasPendingDiskWrites()) return;

        // Frame dispatch holds the event registry's reader lock. Pool recreation
        // registers/unregisters listeners, so commit from a separate SKSE main task.
        const auto tasks = SKSE::GetTaskInterface();
        if (!tasks) { FinishMenuApply(false, "主线程任务接口不可用。"); return; }
        menuApplyScheduled = true;
        const auto alive = menuApplyAlive;
        try {
            tasks->AddTask([this, alive] { if (alive->load()) ExecuteMenuApply(); });
        } catch (const std::exception& e) {
            menuApplyScheduled = false;
            FinishMenuApply(false, e.what());
        }
    }

    void TaskManager::ExecuteMenuApply() {
        menuApplyScheduled = false;
        if (!pendingMenuApply || stoppingComputations.load()) return;
        if (std::chrono::steady_clock::now() - pendingMenuApply->queuedAt > std::chrono::seconds(30)) {
            FinishMenuApply(false, "主线程调度已超时，配置未应用，请重试。");
            return;
        }
        auto& updater = ObjectNormalMapUpdater::GetSingleton();
        if (HasPendingUpdates() || ActorVertexHasher::GetSingleton().HasPendingDetection() ||
            updater.GetCount() > 0 || updater.HasPendingCleanup() ||
            NormalMapStore::GetSingleton().HasPendingDiskWrites()) return;

        const auto before = Config::GetSingleton().GetRuntimeTuning();
        auto desired = *before;
        for (const auto& setting : GetMenuSettingsCatalog())
            setting.assign(desired, setting.read(pendingMenuApply->tuning));
        bool reconfigure = false, regenerate = false;
        std::size_t appliedCount = 0, restartCount = 0;
        for (const auto& setting : GetSettingsCatalog()) {
            if (setting.read(*before) == setting.read(desired)) continue;
            if (setting.policy == ApplyPolicy::Restart) ++restartCount;
            else {
                ++appliedCount;
                reconfigure |= setting.policy == ApplyPolicy::IdleReconfigure;
                regenerate |= setting.policy == ApplyPolicy::Regenerate;
            }
        }
        CopyRestartFields(*before, desired);
        std::string error;
        try {
            if (!Config::GetSingleton().ApplyRuntimeTuning(desired, error)) {
                FinishMenuApply(false, error);
                return;
            }
            spdlog::set_level(Config::GetSingleton().GetLogLevel());
            spdlog::flush_on(Config::GetSingleton().GetFlushLevel());
            if (reconfigure || regenerate) {
                // GeometryProcessing reads these derived thresholds directly.
                weldDistance = std::max(floatPrecision, Config::GetSingleton().GetWeldDistance());
                weldDistanceMult = 1.0f / weldDistance;
                boundaryWeldDistance = std::max(floatPrecision, Config::GetSingleton().GetBoundaryWeldDistance());
                boundaryWeldDistanceMult = 1.0f / boundaryWeldDistance;
                if (reconfigure) InitialSetting();
                ConditionManager::GetSingleton().InitialConditionList();
                static_cast<MultipleConfig*>(&Config::GetSingleton())->LoadConditionFile();
                ConditionManager::GetSingleton().SortConditions();
                if (regenerate && !updater.Init()) {
                    throw std::runtime_error("无法重新建立法线生成资源");
                }
                if (regenerate) RefreshRegisteredActors();
            }
            if (desired.HotKey1 != before->HotKey1 || desired.HotKey2 != before->HotKey2) {
                isPressedHotKey1 = false;
                isResetTasks = false;
            }
            if (desired.DiskCacheLimitMB != before->DiskCacheLimitMB)
                NormalMapStore::GetSingleton().CheckDiskCacheCapacity();
            auto status = "已应用 " + std::to_string(appliedCount) + " 项设置；此操作未写入 INI。";
            if (restartCount) status += "另有 " + std::to_string(restartCount) + " 项需保存到 INI 后重启游戏。";
            FinishMenuApply(true, status);
        } catch (const std::exception& e) {
            std::string ignored;
            Config::GetSingleton().ApplyRuntimeTuning(*before, ignored);
            spdlog::set_level(Config::GetSingleton().GetLogLevel());
            spdlog::flush_on(Config::GetSingleton().GetFlushLevel());
            weldDistance = std::max(floatPrecision, Config::GetSingleton().GetWeldDistance());
            weldDistanceMult = 1.0f / weldDistance;
            boundaryWeldDistance = std::max(floatPrecision, Config::GetSingleton().GetBoundaryWeldDistance());
            boundaryWeldDistanceMult = 1.0f / boundaryWeldDistance;
            try {
                if (reconfigure) InitialSetting();
                if (reconfigure || regenerate) {
                    ConditionManager::GetSingleton().InitialConditionList();
                    static_cast<MultipleConfig*>(&Config::GetSingleton())->LoadConditionFile();
                    ConditionManager::GetSingleton().SortConditions();
                }
                if (regenerate) updater.Init();
            } catch (...) { logger::error("Unable to restore all processing resources after failed settings apply"); }
            FinishMenuApply(false, std::string("应用失败，已恢复原配置：") + e.what());
        }
    }

    TaskManager::~TaskManager() {
        menuApplyAlive->store(false);
        WaitForComputations();
    }

    void TaskManager::PruneCompletedComputations() {
        std::erase_if(pendingComputations, [](auto& computation) {
            return !computation.valid() || computation.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        });
    }

    void TaskManager::WaitForComputations() {
        stoppingComputations.store(true);
        // Computations can wait for more GPU tasks after frame pumping has ended.
        if (gpuTask)
            gpuTask->BeginDrain();
        // Completion callbacks may retain busy tickets until a frame runs. Wait
        // only for computation futures, with all TaskManager members still alive.
        for (auto& computation : pendingComputations)
            if (computation.valid()) computation.wait();
        pendingComputations.clear();
    }

    void TaskManager::onEvent(const QuitGameEvent& e) {
        menuApplyAlive->store(false);
        if (pendingMenuApply) FinishMenuApply(false, "游戏正在退出，配置未应用。");
        WaitForComputations();
    }

	void TaskManager::Init()
	{
		if (const auto Menu = RE::UI::GetSingleton(); Menu)
			Menu->AddEventSink<RE::MenuOpenCloseEvent>(this);
		if (const auto NiNodeEvent = SKSE::GetNiNodeUpdateEventSource(); NiNodeEvent)
			NiNodeEvent->AddEventSink<SKSE::NiNodeUpdateEvent>(this);
		if (const auto inputManager = RE::BSInputDeviceManager::GetSingleton(); inputManager)
			inputManager->AddEventSink<RE::InputEvent*>(this);
	}

	void TaskManager::onEvent(const FrameEvent& e)
	{
        if (stoppingComputations.load())
            return;
        PruneCompletedComputations();
        if (IsSaveLoading.load() || IsInLoading.load()) {
            geometryRetry.Clear();
            return;
        }

		RunDelayTask();
        ProcessMenuApply();
		if (pendingMenuApply) return;
		RunUpdateQueue();
		RunManageResource(false);
	}
	void TaskManager::onEvent(const FacegenNiNodeEvent& e)
	{
		if (!e.facegenNiNode)
			return;
		RE::Actor* actor = skyrim_cast<RE::Actor*>(e.facegenNiNode->GetUserData());
		if (!actor)
			return;

		RE::FormID id = actor->formID;
        geometryRetry.Forget(id);
		std::string name = actor->GetName();
		RegisterDelayTask(Config::GetSingleton().GetUpdateDelayTick(), [this, id, name]() {
			RE::Actor* actor = GetFormByID<RE::Actor*>(id);
			if (IsInvalidActor(actor))
			{
				logger::error("{:x} {} : invalid reference. so skip", id, name);
				return;
			}
			QUpdateNormalMap(actor, BipedObjectSlot::kAll);
		});
	}
	void TaskManager::onEvent(const ActorChangeHeadPartEvent& e)
	{
		if (!e.actor)
			return;

		RE::FormID id = e.actor->formID;
        geometryRetry.Forget(id);
		std::string name = e.actor->GetName();
		RegisterDelayTask(Config::GetSingleton().GetUpdateDelayTick(), [this, id, name]() {
			RE::Actor* actor = GetFormByID<RE::Actor*>(id);
			if (IsInvalidActor(actor))
			{
				logger::error("{:x} {} : invalid reference. so skip", id, name);
				return;
			}
            QUpdateNormalMap(actor, BipedObjectSlot::kAll);
		});
	}
	void TaskManager::onEvent(const ArmorAttachEvent& e)
	{
		if (!e.actor)
			return;
		if (!e.hasAttached)
			return;

		RE::FormID id = e.actor->formID;
        geometryRetry.Forget(id);
		std::string name = e.actor->GetName();
		RegisterDelayTask(Config::GetSingleton().GetUpdateDelayTick(), [this, id, name, e]() {
			RE::Actor* actor = GetFormByID<RE::Actor*>(id);
			if (IsInvalidActor(actor))
			{
				logger::error("{:x} {} : invalid reference. so skip", id, name);
				return;
			}
            QUpdateNormalMap(actor, kAll);
		});
	}

	void TaskManager::RunUpdateQueue()
	{
		if (Config::GetSingleton().GetQueueTime())
			PerformanceLog(std::string("updateSlotQueue"), false, false);

		if (!isAfterLoading && !isRevertDone)
		{
            if (!SetImmediately(false))
                return;
            isRevertDone = true;
            logger::info("processing in loading screen done");
		}

		auto camera = RE::PlayerCamera::GetSingleton();
		if (!camera)
			return;
		auto cameraPosition = camera->GetRuntimeData2().pos;

        const auto tuning = Config::GetSingleton().GetRuntimeTuning();
        const float updateDistance = tuning->updateDistance * tuning->updateDistance;
        // Re-evaluate actors when switching from a finite range to unlimited as well.
        {
            const bool vramSave = Config::GetSingleton().GetUpdateDistanceVramSave();
            std::vector<std::function<void()>> queues;
            {
                std::lock_guard lg(isActiveActorsLock);
                for (auto it = isActiveActors.begin(); it != isActiveActors.end();)
                {
                    RE::Actor* actor = GetFormByID<RE::Actor*>(it->first);
                    if (IsInvalidActor(actor))
                    {
                        const auto actorID = it->first;
                        it = isActiveActors.erase(it);
                        RemoveLastNormalMap(actorID);
                        geometryRetry.Forget(actorID);
                        continue;
                    }
					if (IsPlayer(actor->formID))
					{
                        it->second = true;
                        it++;
                        continue;
					}
                    bool isInRange = false;
                    if (updateDistance <= floatPrecision)
                    {
                        isInRange = true;
                    }
                    else
                    {
                        isInRange = cameraPosition.GetSquaredDistance(actor->loadedData->data3D->world.translate) <= updateDistance;
                    }
                    if (it->second != isInRange)
                    {
                        if (!isInRange)
                        {
                            if (vramSave)
                            {
                                if (IsExistLastNormalMap(actor->formID))
                                {
                                    if (GetIsUpdating(actor->formID))
                                    {
                                        it++;
                                        continue;
                                    }
                                    RemoveNormalMap(actor);
                                }
                            }
                        }
                        else
                        {
                            queues.push_back([this, actor]() { QUpdateNormalMap(actor); });
                        }
                        it->second = isInRange;
                    }
                    it++;
                }
            }
            for (auto& q : queues)
            {
                q();
			}
        }

        UpdateSlotQueue updateSlotQueue_;
        {
            std::lock_guard lg(updateSlotQueueLock);
            updateSlotQueue_ = std::move(updateSlotQueue);
        }

        // Live scene graphs and NiTexture writes stay on the frame thread.
        // Round-robin bounds per-frame scans without starving deferred actors.
        const auto scanStart = GeometryRetry::Clock::now();
        unsigned scans = 0;
        std::vector<std::pair<RE::FormID, bSlotbit>> ordered;
        auto split = updateSlotQueue_.upper_bound(lastScanActor);
        ordered.insert(ordered.end(), split, updateSlotQueue_.end());
        ordered.insert(ordered.end(), updateSlotQueue_.begin(), split);
        for (auto& map : ordered) {
                RE::Actor* actor = GetFormByID<RE::Actor*>(map.first);
                if (IsInvalidActor(actor)) {
                    geometryRetry.Forget(map.first);
                    continue;
                }
                if (!IsActorEnabled(actor)) continue;
                if (GetIsActiveActor(actor->formID))
                {
                    if (!GetIsUpdating(actor->formID))
                    {
                        if (!ConditionManager::GetSingleton().GetCondition(actor).Enable)
                            continue;
                        auto root = actor->loadedData->data3D.get();
                        auto race = actor->GetRace();
                        const auto retryState = geometryRetry.Check(actor->formID, reinterpret_cast<std::uintptr_t>(root), race ? race->formID : 0);
                        // Drop the request entirely after three spaced empty scans.
                        // Model events or explicit refresh enqueue it again and clear the mark.
                        if (retryState == GeometryRetry::State::Blacklisted)
                            continue;
                        if (retryState == GeometryRetry::State::CoolingDown ||
                            scans >= 2 || (scans && GeometryRetry::Clock::now() - scanStart >= std::chrono::milliseconds(2))) {
                            std::lock_guard lg(updateSlotQueueLock);
                            updateSlotQueue[actor->formID] |= map.second;
                            continue;
                        }
                        ++scans;
                        lastScanActor = actor->formID;
                        auto geometries = GetAllGeometries(actor);
                        if (geometries.empty()) {
                            const bool blacklisted = geometryRetry.Failed(actor->formID, reinterpret_cast<std::uintptr_t>(root), race ? race->formID : 0);
                            logger::debug("{:x} : no supported geometry; {}", actor->formID, blacklisted ? "session model blacklist" : "retry after cooldown");
                            if (!blacklisted) {
                                std::lock_guard lg(updateSlotQueueLock);
                                updateSlotQueue[actor->formID] |= map.second;
                            }
                        } else {
                            geometryRetry.Forget(actor->formID);
                            QUpdateNormalMapImpl(actor, std::move(geometries), map.second);
                        }
                    }
                    else
                    {
                        std::lock_guard lg(updateSlotQueueLock);
                        updateSlotQueue[actor->formID] |= map.second;
                    }
                }
        }

        if (isAfterLoading)
        {
            isAfterLoading = false;
            if (SetImmediately(false))
            {
                isRevertDone = true;
                logger::info("processing in loading screen done");
            }
        }

		if (Config::GetSingleton().GetQueueTime() && updateSlotQueue_.size() > 0)
            PerformanceLog(std::string("updateSlotQueue"), true, false, updateSlotQueue_.size());
	}

	void TaskManager::RunDelayTask()
	{
		auto delayTask_ = std::move(deferredCallbacks);
		deferredCallbacks.clear();
		auto ready = delayTasks.TakeReady();
		delayTask_.insert(delayTask_.end(), std::make_move_iterator(ready.begin()), std::make_move_iterator(ready.end()));
		if (delayTask_.empty()) return;
		if (Config::GetSingleton().GetQueueTime())
			PerformanceLog(std::string(__func__), false, false);
        // Completion callbacks can write live materials; never dispatch them via TBB.
        const auto callbackStart = GeometryRetry::Clock::now();
        std::size_t completed = 0;
        for (; completed < delayTask_.size(); ++completed) {
            if (completed >= 4 || (completed && GeometryRetry::Clock::now() - callbackStart >= std::chrono::milliseconds(2)))
                break;
            try { delayTask_[completed](); }
            catch (const std::exception& e) { logger::error("Delayed task failed: {}", e.what()); }
            catch (...) { logger::error("Delayed task failed with unknown exception"); }
        }
        if (completed < delayTask_.size()) {
            deferredCallbacks.insert(deferredCallbacks.end(), std::make_move_iterator(delayTask_.begin() + completed), std::make_move_iterator(delayTask_.end()));
        }

		if (Config::GetSingleton().GetQueueTime())
			PerformanceLog(std::string(__func__), true, false, delayTask_.size());
	}
	void TaskManager::RegisterDelayTask(std::function<void()> func) 
	{
		delayTasks.Enqueue(0, std::move(func));
	}
	void TaskManager::RegisterDelayTask(std::int16_t delayTick, std::function<void()> func) 
	{
		delayTasks.Enqueue(delayTick, std::move(func));
	}

	void TaskManager::RunManageResource(bool isImminently)
	{
		static std::clock_t lastTickTime = currentTime;
		if (lastTickTime + 1000 > currentTime && !isImminently) //1sec
			return;
		lastTickTime = currentTime;
		ReleaseResourceOnUnloadActors();
	}

	TaskManager::GeometryList TaskManager::GetAllGeometries(RE::Actor* a_actor)
	{
        GeometryList geometries;
		if (IsInvalidActor(a_actor))
			return geometries;
		auto root = a_actor->loadedData->data3D.get();
		if (!root)
            return geometries;
		RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geometry) -> RE::BSVisit::BSVisitControl {
			using Feature = RE::BSShaderMaterial::Feature;
			if (!geometry || geometry->name.empty())
				return RE::BSVisit::BSVisitControl::kContinue;
			if (IsContainString(geometry->name.c_str(), "[Ovl") || IsContainString(geometry->name.c_str(), "[SOvl") || IsContainString(geometry->name.c_str(), "overlay")) //without overlay
				return RE::BSVisit::BSVisitControl::kContinue;
			if (!geometry->GetGeometryRuntimeData().shaderProperty)
				return RE::BSVisit::BSVisitControl::kContinue;
            auto effect = geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (!effect)
				return RE::BSVisit::BSVisitControl::kContinue;
			auto lightingShader = netimmerse_cast<RE::BSLightingShaderProperty*>(effect);
			if (!lightingShader || !lightingShader->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals))
				return RE::BSVisit::BSVisitControl::kContinue;
            if (auto alphaProperty = geometry->GetGeometryRuntimeData().alphaProperty.get(); alphaProperty)
                return RE::BSVisit::BSVisitControl::kContinue;
            auto hash = ActorVertexHasher::Hash(geometry);
            geometries.insert(std::make_pair(hash.GetHash(), geometry));
			return RE::BSVisit::BSVisitControl::kContinue;
        });
		return geometries;
	}
    bool TaskManager::QUpdateNormalMap(RE::Actor* a_actor, bSlotbit bipedSlot)
    {
		if (stoppingComputations.load())
            return false;
        if (!a_actor || bipedSlot == 0)
            return false;
        RE::FormID id = a_actor->formID;
        std::string actorName = a_actor->GetName();
        if (IsPlayer(id) && !Config::GetSingleton().GetPlayerEnable())
            return true;
        if (!IsPlayer(id) && !Config::GetSingleton().GetNPCEnable())
            return true;
        if (GetSex(a_actor) == RE::SEX::kMale && !Config::GetSingleton().GetMaleEnable())
            return true;
        if (GetSex(a_actor) == RE::SEX::kFemale && !Config::GetSingleton().GetFemaleEnable())
            return true;
        if (bipedSlot & BipedObjectSlot::kHead && !Config::GetSingleton().GetHeadEnable())
            bipedSlot &= ~BipedObjectSlot::kHead;
        {
            std::lock_guard lg(updateSlotQueueLock);
            updateSlotQueue[id] |= bipedSlot;
        }
        SetIsActiveActor(id, true);
        logger::debug("{}::{:x}::{} : queue added 0x{:x}", __func__, id, actorName, bipedSlot);
        return true;
    }

	bool TaskManager::QUpdateNormalMapImpl(RE::Actor* a_actor, GeometryList a_srcGeometies, bSlotbit bipedSlot)
	{
		if (stoppingComputations.load())
            return false;
		if (!a_actor || bipedSlot == 0)
			return false;
        if (!IsActorEnabled(a_actor)) return true;

        auto condition = ConditionManager::GetSingleton().GetCondition(a_actor);
        if (!condition.Enable)
            return true;

		if (a_srcGeometies.empty())
		{
			logger::error("{}::{:x}::{} : no valid geometry found", __func__, a_actor->formID, a_actor->GetName());
			return false;
		}

		logger::debug("{}::{:x}::{} : get geometry datas...", __func__, a_actor->formID, a_actor->GetName());
		RE::FormID id = a_actor->formID;
		std::string actorName = a_actor->GetName();
		float detailStrength = condition.DetailStrength;
        if (auto found = Papyrus::detailStrengthMap.find(id); found != Papyrus::detailStrengthMap.end())
            detailStrength = found->second;
		auto gender = GetSex(a_actor);
		GeometryDataPtr newGeometryData = std::make_shared<GeometryData>();
		UpdateSet newUpdateSet;
		for (auto& pair : a_srcGeometies)
        {
            auto geo = pair.second;
			using Feature = RE::BSShaderMaterial::Feature;
			if (!geo || geo->name.empty())
                continue;
			if (auto extraData = geo->GetExtraData<RE::NiIntegerExtraData>(NoDynamicNormalMapExtraDataName); extraData && extraData->value > 0)
				continue;
			if (!geo->GetGeometryRuntimeData().shaderProperty)
				continue;
            auto effect = geo->GetGeometryRuntimeData().shaderProperty.get();
			auto lightingShader = netimmerse_cast<RE::BSLightingShaderProperty*>(effect);
			if (!lightingShader || !lightingShader->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals))
				continue;
            if (auto alphaProperty = geo->GetGeometryRuntimeData().alphaProperty.get(); alphaProperty)
                continue;
			RE::BSLightingShaderMaterialBase* material = skyrim_cast<RE::BSLightingShaderMaterialBase*>(lightingShader->material);
			if (!material || !material->normalTexture || material->normalTexture->name.empty())
				continue;
			std::string texturePath = GetOriginalTexturePath(lowLetter(material->normalTexture->name.c_str()));
			if (texturePath.empty() || !IsExistFile(texturePath, ExistType::textures))
			{
				if (material->textureSet)
				{
					texturePath = GetTexturePath(material->textureSet.get(), RE::BGSTextureSet::Textures::kNormal);
					if (texturePath.empty() || !IsExistFile(texturePath, ExistType::textures))
						continue;
				}
			}
			if (!geo->GetGeometryRuntimeData().skinInstance)
                continue;

			bSlot slot = nif::GetBipedSlot(geo, condition.DynamicTriShapeAsHead);
			if ((!condition.HeadEnable || !Config::GetSingleton().GetHeadEnable()) && slot == RE::BIPED_OBJECT::kHead)
				continue;

			newGeometryData->CopyGeometryData(geo);

			if (bipedSlot & 1 << slot)
            {
                UpdateTextureSet newSet;
                newSet.slot = slot;
                newSet.geometryName = geo->name.c_str();
                newSet.textureName = GetTextureName(a_actor, slot, texturePath);
                newSet.srcTexturePath = texturePath;

				std::string saveDetailTexturePath = GetDetailNormalMapPath(a_actor);
                newSet.detailTexturePath = saveDetailTexturePath.empty() ? GetDetailNormalMapPath(texturePath, condition.ProxyDetailTextureFolder, condition.ProxyFirstScan) : saveDetailTexturePath;

				std::string saveOverlayTexturePath = GetOverlayNormalMapPath(a_actor);
                newSet.overlayTexturePath = saveOverlayTexturePath.empty() ? GetOverlayNormalMapPath(texturePath, condition.ProxyOverlayTextureFolder, condition.ProxyFirstScan) : saveOverlayTexturePath;

				std::string saveMaskTexturePath = GetMaskNormalMapPath(a_actor);
                newSet.maskTexturePath = saveMaskTexturePath.empty() ? GetMaskNormalMapPath(texturePath, condition.ProxyMaskTextureFolder, condition.ProxyFirstScan) : saveMaskTexturePath;

				newSet.detailStrength = detailStrength;

				logger::debug("{:x}::{} : {} - queue added on update object normalmap", id, actorName, geo->name.c_str());

				if (!Config::GetSingleton().GetRevertNormalMap())
                {
                    if (IsExistLastNormalMap(id, newSet.slot, newSet.textureName))
                        Shader::TextureLoadManager::CreateSourceTexture(newSet.textureName, material->normalTexture);
                }
                newUpdateSet.push_back(std::make_pair(geo, std::move(newSet)));

				ActorVertexHasher::GetSingleton().Register(a_actor, geo);

				if (overrideInterfaceAPI->OverrideInterfaceload())
				{
					if (Config::GetSingleton().GetRemoveSkinOverrides())
					{
						overrideInterfaceAPI->RemoveSkinOverride(a_actor, gender == RE::SEX::kFemale, false, 1 << slot, OverrideInterfaceAPI::Key::ShaderTexture, RE::BGSTextureSet::Textures::kNormal);
						logger::info("{:x}::{} : {} - remove skin override for normalmap update", id, actorName, geo->name.c_str());
					}
					if (Config::GetSingleton().GetRemoveNodeOverrides())
					{
						overrideInterfaceAPI->RemoveNodeOverride(a_actor, gender == RE::SEX::kFemale, geo->name, OverrideInterfaceAPI::Key::ShaderTexture, RE::BGSTextureSet::Textures::kNormal);
						logger::info("{:x}::{} : {} - remove node override for normalmap update", id, actorName, geo->name.c_str());
					}
                }
			}
        }

		QUpdateNormalMapImpl(a_actor->formID, actorName, newGeometryData, newUpdateSet, a_actor->loadedData->data3D);
		return true;
	}
	void TaskManager::QUpdateNormalMapImpl(RE::FormID a_actorID, std::string a_actorName, GeometryDataPtr a_geoData, UpdateSet& a_updateSet, RE::NiPointer<RE::NiAVObject> sourceRoot)
	{
        if (stoppingComputations.load() || !a_geoData || a_updateSet.empty() || GetIsUpdating(a_actorID))
			return;

        // Keep the busy state until the frame-thread completion is consumed.
        auto busyTicket = ActorWorkState::Claim(actorWorkState, a_actorID);
        auto func = [this, a_actorID, a_actorName, a_geoData, sourceRoot, busyTicket = std::move(busyTicket), a_updateSet = std::move(a_updateSet)]() mutable {
          // A stored future retains its callable; the frame completion owns busy.
          auto activeTicket = std::move(busyTicket);
          try {
            if (Config::GetSingleton().GetFullUpdateTime())
                PerformanceLog(std::string("QUpdateNormalMapImpl") + "::" + SetHex(a_actorID, false), false, false);

            if (!ObjectNormalMapUpdater::GetSingleton().CreateGeometryResourceData(a_actorID, a_geoData))
            {
                logger::error("{:x}::{} : Failed to get geometry data", a_actorID, a_actorName);
                return;
            }

            ObjectNormalMapUpdater::UpdateResult textures;
            if (Config::GetSingleton().GetGPUEnable())
                textures = ObjectNormalMapUpdater::GetSingleton().UpdateObjectNormalMapGPU(a_actorID, a_geoData, a_updateSet);
            else
                textures = ObjectNormalMapUpdater::GetSingleton().UpdateObjectNormalMap(a_actorID, a_geoData, a_updateSet);
            if (textures.empty())
            {
                logger::error("{:x}::{} : Failed to update object normalmap", a_actorID, a_actorName);
                return;
            }

            RegisterDelayTask([this, a_actorID, a_actorName, sourceRoot, busyTicket = activeTicket, textures = std::move(textures)]() {
                auto actor = GetFormByID<RE::Actor*>(a_actorID);
                if (IsInvalidActor(actor))
                {
                    logger::error("{:x}::{} : invalid reference", a_actorID, a_actorName);
                    return;
                }

                auto root = actor->loadedData->data3D.get();
                if (root != sourceRoot.get())
                {
                    // A load or 3D rebuild replaced the graph while work was in flight.
                    // Retain the current materials and request fresh data next frame.
                    geometryRetry.Forget(a_actorID);
                    QUpdateNormalMap(actor, BipedObjectSlot::kAll);
                    return;
                }
                std::unordered_map<std::string, RE::NiSourceTexturePtr> createdTextures;
                RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geo) -> RE::BSVisit::BSVisitControl {
                    using Feature = RE::BSShaderMaterial::Feature;
                    if (!geo || geo->name.empty())
                        return RE::BSVisit::BSVisitControl::kContinue;
                    if (auto extraData = geo->GetExtraData<RE::NiIntegerExtraData>(NoDynamicNormalMapExtraDataName); extraData && extraData->value == 2)
                        return RE::BSVisit::BSVisitControl::kContinue;

                    auto found = textures.end();
                    if (IsContainString(geo->name.c_str(), "[Ovl") || IsContainString(geo->name.c_str(), "[SOvl") || IsContainString(geo->name.c_str(), "overlay"))
                    {
                        if (!Config::GetSingleton().GetApplyOverlay())
                            return RE::BSVisit::BSVisitControl::kContinue;

                        bSlot slot = 0;
                        if (IsContainString(geo->name.c_str(), "Body"))
                            slot = RE::BIPED_OBJECT::kBody;
                        else if (IsContainString(geo->name.c_str(), "Hands"))
                            slot = RE::BIPED_OBJECT::kHands;
                        else if (IsContainString(geo->name.c_str(), "Feet"))
                            slot = RE::BIPED_OBJECT::kFeet;
                        else if (IsContainString(geo->name.c_str(), "overlay") || IsContainString(geo->name.c_str(), "Face"))
                            slot = RE::BIPED_OBJECT::kHead;
                        else
                            return RE::BSVisit::BSVisitControl::kContinue;

                        found = std::find_if(textures.begin(), textures.end(), [&](const ObjectNormalMapUpdater::NormalMapResult& normalmap) {
                            return normalmap.slot == slot &&
                                   !IsContainString(normalmap.geoName, "Vagina") && !IsContainString(normalmap.geoName, "Anus") && !IsContainString(normalmap.geoName, "Canal");
                        });
                    }
                    else
                    {
                        found = std::find_if(textures.begin(), textures.end(), [&](const ObjectNormalMapUpdater::NormalMapResult& normalmap) {
                            return normalmap.geometry == geo;
                        });
                    }

                    auto effect = geo->GetGeometryRuntimeData().shaderProperty.get();
                    if (!effect)
                        return RE::BSVisit::BSVisitControl::kContinue;
                    auto lightingShader = netimmerse_cast<RE::BSLightingShaderProperty*>(effect);
                    if (!lightingShader || !lightingShader->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals))
                        return RE::BSVisit::BSVisitControl::kContinue;
                    RE::BSLightingShaderMaterialBase* material = skyrim_cast<RE::BSLightingShaderMaterialBase*>(lightingShader->material);
                    if (!material || !material->normalTexture)
                        return RE::BSVisit::BSVisitControl::kContinue;

                    if (found == textures.end())
                    {
                        auto& skinInstance = geo->GetGeometryRuntimeData().skinInstance;
                        if (!skinInstance)
                            return RE::BSVisit::BSVisitControl::kContinue;

                        auto dismember = netimmerse_cast<RE::BSDismemberSkinInstance*>(skinInstance.get());
                        if (dismember)
                        {
                            std::string texturePath = GetOriginalTexturePath(lowLetter(material->normalTexture->name.c_str()));
                            found = std::find_if(textures.begin(), textures.end(), [&](const ObjectNormalMapUpdater::NormalMapResult& normalmap) {
                                for (std::int32_t p = 0; p < dismember->GetRuntimeData().numPartitions; p++)
                                {
                                    bSlot slot;
                                    auto pslot = dismember->GetRuntimeData().partitions[p].slot;
                                    if (pslot < 30 || pslot >= RE::BIPED_OBJECT::kEditorTotal + 30)
                                    {
                                        if (geo->AsDynamicTriShape())
                                        { // maybe head
                                            slot = RE::BIPED_OBJECT::kHead;
                                        }
                                        else if (pslot == 0) // BP_TORSO
                                        {
                                            slot = RE::BIPED_OBJECT::kBody;
                                        }
                                        else // unknown slot
                                            continue;
                                    }
                                    else
                                        slot = pslot - 30;
                                    if (normalmap.slot == slot && IsSameString(normalmap.texturePath, texturePath) && !IsSameString(normalmap.geoName, geo->name.c_str()))
                                        return true;
                                }
                                return false;
                            });
                        }
                        if (found == textures.end())
                            return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    if (!found->texture || !found->texture->normalmapTexture2D || !found->texture->normalmapShaderResourceView)
                        return RE::BSVisit::BSVisitControl::kContinue;

                    RE::NiSourceTexturePtr normalmap = nullptr;
                    if (auto texIt = createdTextures.find(found->textureName); texIt != createdTextures.end())
                    {
                        normalmap = texIt->second;
                    }
                    else
                    {
                        if (Shader::TextureLoadManager::GetSingleton().CreateNiTexture(found->textureName, found->texture->normalmapTexture2D, found->texture->normalmapShaderResourceView, normalmap) < 0 || !normalmap || !normalmap->rendererTexture || !normalmap->rendererTexture->texture || !normalmap->rendererTexture->resourceView)
                        {
                            logger::error("{:x}::{}::{} : Failed to create NiTexture", a_actorID, a_actorName, geo->name.c_str());
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        createdTextures.insert(std::make_pair(found->textureName, normalmap));
                        if (Config::GetSingleton().GetLogLevel() <= spdlog::level::level_enum::debug)
                        {
                            TextureLog(found->texture->normalmapTexture2D.Get());
                            TextureLog(found->texture->normalmapShaderResourceView.Get());
                        }
                    }

                    if (Config::GetSingleton().GetDebugTexture())
                        material->diffuseTexture = normalmap;
                    material->normalTexture = normalmap;
					ActorVertexHasher::GetSingleton().RegisterCheckTexture(actor, geo);
                    InsertLastNormalMap(a_actorID, found->slot, found->textureName);
                    logger::info("{:x}::{}::{} : update object normalmap done", a_actorID, a_actorName, geo->name.c_str());
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
            });

            if (Config::GetSingleton().GetFullUpdateTime())
                PerformanceLog(std::string("QUpdateNormalMapImpl") + "::" + SetHex(a_actorID, false), true, false);
          } catch (const std::exception& e) {
            logger::error("{:x} : normal-map task failed: {}", a_actorID, e.what());
          } catch (...) {
            logger::error("{:x} : normal-map task failed with unknown exception", a_actorID);
          }
        };
        if (isImmediately)
        {
            currentActorThreads.load()->submitAsync(std::move(func)).get();
        }
        else {
            // Allocate ownership before dispatch so an allocation failure cannot
            // leave an untracked computation referring to this TaskManager.
            pendingComputations.emplace_back();
            pendingComputations.back() = currentActorThreads.load()->submitAsync(std::move(func));
        }
	}

	std::string TaskManager::GetDetailNormalMapPath(std::string a_normalMapPath)
	{
		constexpr std::string_view prefix = "Textures\\";
		constexpr std::string_view msn_suffix = "_msn";
		constexpr std::string_view n_suffix = "_n";
		if (a_normalMapPath.empty())
			return "";
		if (!stringStartsWith(a_normalMapPath, prefix.data()))
			a_normalMapPath = prefix.data() + a_normalMapPath;
		std::filesystem::path file(a_normalMapPath);
		std::string filename = file.stem().string();
		if (stringEndsWith(filename, msn_suffix.data())) //_msn -> _n
		{
			filename = stringRemoveEnds(filename, msn_suffix.data());
			filename += n_suffix;
			std::string fullPath = (file.parent_path() / (filename + ".dds")).string();
			if (IsExistFile(fullPath, ExistType::textures))
				return fullPath;
		}
		else if (stringEndsWith(filename, n_suffix.data())) //_n
		{
			if (IsExistFile(a_normalMapPath, ExistType::textures))
				return a_normalMapPath;
		}
		return "";
	}
	std::string TaskManager::GetDetailNormalMapPath(std::string a_normalMapPath, std::vector<std::string> a_proxyFolder, bool a_proxyFirstScan)
	{
		std::string result;
		if (!a_proxyFirstScan)
		{
			result = GetDetailNormalMapPath(a_normalMapPath);
			if (!result.empty() || a_proxyFolder.empty())
				return result;
		}
		std::filesystem::path file(a_normalMapPath);
		std::string filename = file.stem().string();
		for (auto& folder : a_proxyFolder)
		{
			result = GetDetailNormalMapPath(folder + "\\" + filename + ".dds");
			if (!result.empty())
				return result;
		}
		if (a_proxyFirstScan)
		{
			result = GetDetailNormalMapPath(a_normalMapPath);
			if (!result.empty())
				return result;
		}
		return "";
	}
	std::string TaskManager::GetDetailNormalMapPath(RE::Actor* a_actor)
	{
		if (!a_actor)
			return "";
		auto found = Papyrus::normalmaps[Papyrus::normalmapTypes::detail].find(a_actor->formID);
		if (found == Papyrus::normalmaps[Papyrus::normalmapTypes::detail].end())
			return "";
		return found->second;
	}

	std::string TaskManager::GetOverlayNormalMapPath(std::string a_normalMapPath)
	{
		constexpr std::string_view prefix = "Textures\\";
		constexpr std::string_view msn_suffix = "_msn";
		constexpr std::string_view n_suffix = "_n";
		constexpr std::string_view ov_suffix = "_ov";
		if (a_normalMapPath.empty())
			return "";
		if (!stringStartsWith(a_normalMapPath, prefix.data()))
			a_normalMapPath = prefix.data() + a_normalMapPath;
		std::filesystem::path file(a_normalMapPath);
		std::string filename = file.stem().string();

		if (stringEndsWith(filename, msn_suffix.data())) //_msn -> _n_ov
		{
			std::string filename_n_ov = stringRemoveEnds(filename, msn_suffix.data());
			filename_n_ov += n_suffix;
			filename_n_ov += ov_suffix;
			std::string fullPath = (file.parent_path() / (filename_n_ov + ".dds")).string();
			if (IsExistFile(fullPath, ExistType::textures))
				return fullPath;
		}
		if (stringEndsWith(filename, n_suffix.data())) //_n -> _n_ov
		{
			std::string filename_n_ov = filename + ov_suffix.data();
			std::string fullPath = (file.parent_path() / (filename_n_ov + ".dds")).string();
			if (IsExistFile(fullPath, ExistType::textures))
				return fullPath;
		}
		return "";
	}
	std::string TaskManager::GetOverlayNormalMapPath(std::string a_normalMapPath, std::vector<std::string> a_proxyFolder, bool a_proxyFirstScan)
	{
		std::string result;
		if (!a_proxyFirstScan)
		{
			result = GetOverlayNormalMapPath(a_normalMapPath);
			if (!result.empty() || a_proxyFolder.empty())
				return result;
		}
		std::filesystem::path file(a_normalMapPath);
		std::string filename = file.stem().string();
		for (auto& folder : a_proxyFolder)
		{
			result = GetOverlayNormalMapPath(folder + "\\" + filename + ".dds");
			if (!result.empty())
				return result;
		}
		if (a_proxyFirstScan)
		{
			result = GetOverlayNormalMapPath(a_normalMapPath);
			if (!result.empty())
				return result;
		}
		return "";
	}
	std::string TaskManager::GetOverlayNormalMapPath(RE::Actor* a_actor)
	{
		if (!a_actor)
			return "";
		auto found = Papyrus::normalmaps[Papyrus::normalmapTypes::overlay].find(a_actor->formID);
		if (found == Papyrus::normalmaps[Papyrus::normalmapTypes::overlay].end())
			return "";
		return found->second;
	}

	std::string TaskManager::GetMaskNormalMapPath(std::string a_normalMapPath)
	{
		constexpr std::string_view prefix = "Textures\\";
		constexpr std::string_view msn_suffix = "_msn";
		constexpr std::string_view n_suffix = "_n";
		constexpr std::string_view m_suffix = "_m";
		if (a_normalMapPath.empty())
			return "";
		if (!stringStartsWith(a_normalMapPath, prefix.data()))
			a_normalMapPath = prefix.data() + a_normalMapPath;
		std::filesystem::path file(a_normalMapPath);
		std::string filename = file.stem().string();

		if (stringEndsWith(filename, msn_suffix.data())) //_msn -> _n_m
		{
			std::string filename_n_m = stringRemoveEnds(filename, msn_suffix.data());
			filename_n_m += n_suffix;
			filename_n_m += m_suffix;
			std::string fullPath = (file.parent_path() / (filename_n_m + ".dds")).string();
			if (IsExistFile(fullPath, ExistType::textures))
				return fullPath;
		}
		if (stringEndsWith(filename, n_suffix.data())) //_n -> _n_m
		{
			std::string filename_n_m = filename + m_suffix.data();
			std::string fullPath = (file.parent_path() / (filename_n_m + ".dds")).string();
			if (IsExistFile(fullPath, ExistType::textures))
				return fullPath;
		}
		return "";
	}
	std::string TaskManager::GetMaskNormalMapPath(std::string a_normalMapPath, std::vector<std::string> a_proxyFolder, bool a_proxyFirstScan)
	{
		std::string result;
		if (!a_proxyFirstScan)
		{
			result = GetMaskNormalMapPath(a_normalMapPath);
			if (!result.empty() || a_proxyFolder.empty())
				return result;
		}
		std::filesystem::path file(a_normalMapPath);
		std::string filename = file.stem().string();
		for (auto& folder : a_proxyFolder)
		{
			result = GetMaskNormalMapPath(folder + "\\" + filename + ".dds");
			if (!result.empty())
				return result;
		}
		if (a_proxyFirstScan)
		{
			result = GetMaskNormalMapPath(a_normalMapPath);
			if (!result.empty())
				return result;
		}
		return "";
	}
	std::string TaskManager::GetMaskNormalMapPath(RE::Actor* a_actor)
	{
		if (!a_actor)
			return "";
		auto found = Papyrus::normalmaps[Papyrus::normalmapTypes::mask].find(a_actor->formID);
		if (found == Papyrus::normalmaps[Papyrus::normalmapTypes::mask].end())
			return "";
		return found->second;
	}

	std::string TaskManager::FixTexturePath(std::string texturePath)
	{
		texturePath = FixPath(texturePath);
		texturePath = stringRemoveStarts(texturePath, "data\\");
		if (!stringStartsWith(texturePath, "textures\\"))
			texturePath = "textures\\" + texturePath;
		return texturePath;
	}

	std::string TaskManager::GetTextureName(RE::Actor* a_actor, bSlot a_bipedSlot, std::string a_texturePath)
	{ // ActorID + BipedSlot + TexturePath
		if (!a_actor || a_texturePath.empty())
			return "";
        a_texturePath = stringRemoveStarts(a_texturePath, "data\\");
        if (!stringStartsWith(a_texturePath, "textures\\"))
            a_texturePath = "textures\\" + a_texturePath;
		return MDNMPrefix + GetHexStr(a_actor->formID) + "::" + std::to_string(a_bipedSlot) + "::" + a_texturePath;
	}
	bool TaskManager::GetTextureInfo(std::string a_textureName, TextureInfo& a_textureInfo)
	{ // ActorID + BipedSlot + TexturePath
		if (a_textureName.empty() || !IsCreatedByMDNM(a_textureName))
			return false;
		a_textureName = stringRemoveStarts(a_textureName, MDNMPrefix);
		auto frag = split(a_textureName, "::");
		if (frag.size() < 3)
			return false;
		a_textureInfo.actorID = GetHex(frag[0]);
		a_textureInfo.bipedSlot = Config::GetUIntValue(frag[1]);
		a_textureInfo.texturePath = frag[2];
		return true;
	}
	std::string TaskManager::GetOriginalTexturePath(std::string a_textureName)
	{
		TextureInfo info;
		if (GetTextureInfo(a_textureName, info))
			return info.texturePath;
		return FixTexturePath(a_textureName);
	}

	bool TaskManager::RemoveNormalMap(RE::Actor* a_actor)
    {
        if (IsInvalidActor(a_actor))
            return false;

		if (!IsExistLastNormalMap(a_actor->formID))
            return true;

        std::vector<RE::BSGeometry*> restoreGeometries;
        RE::BSVisit::TraverseScenegraphGeometries(a_actor->loadedData->data3D.get(), [&](RE::BSGeometry* geometry) {
            if (geometry) restoreGeometries.push_back(geometry);
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        for (auto geo : restoreGeometries)
        {
            using Feature = RE::BSShaderMaterial::Feature;
            if (!geo || geo->name.empty())
                continue;
            if (!geo->GetGeometryRuntimeData().shaderProperty)
                continue;
            auto effect = geo->GetGeometryRuntimeData().shaderProperty.get();
            auto lightingShader = netimmerse_cast<RE::BSLightingShaderProperty*>(effect);
            if (!lightingShader || !lightingShader->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals))
                continue;
            RE::BSLightingShaderMaterialBase* material = skyrim_cast<RE::BSLightingShaderMaterialBase*>(lightingShader->material);
            if (!material || !material->normalTexture || !material->textureSet)
                continue;
            // Include generated overlays, but leave unrelated mods' replacements alone.
            if (material->normalTexture->name.empty() || !IsCreatedByMDNM(material->normalTexture->name.c_str()))
                continue;
            std::string texturePath = lowLetter(GetTexturePath(material->textureSet.get(), RE::BSTextureSet::Texture::kNormal));
            if (texturePath.empty())
                continue;
            RE::NiPointer<RE::NiSourceTexture> texture;
            Shader::TextureLoadManager::GetSingleton().LoadTexture(texturePath.c_str(), 1, texture, false);
            if (!texture)
                continue;
            material->normalTexture = texture;
        }
        RemoveLastNormalMap(a_actor->formID);
        return true;
    }

	EventResult TaskManager::ProcessEvent(const SKSE::NiNodeUpdateEvent* evn, RE::BSTEventSource<SKSE::NiNodeUpdateEvent>*)
	{
		if (!evn || !evn->reference)
			return EventResult::kContinue;
		auto actor = skyrim_cast<RE::Actor*>(evn->reference);
		if (!actor)
			return EventResult::kContinue; 
		RE::FormID id = actor->formID;
        geometryRetry.Forget(id);
		std::string name = actor->GetName();
		RegisterDelayTask(Config::GetSingleton().GetUpdateDelayTick(), [this, id, name]() {
			RE::Actor* actor = GetFormByID<RE::Actor*>(id);
			if (!actor || !actor->loadedData || !actor->loadedData->data3D)
			{
				logger::error("{:x} {} : invalid reference. so skip", id, name);
				return;
			}
			QUpdateNormalMap(actor, BipedObjectSlot::kAll);
		});
		return EventResult::kContinue;
	}

	EventResult TaskManager::ProcessEvent(RE::InputEvent* const* evn, RE::BSTEventSource<RE::InputEvent*>*)
	{
		if (!evn || !*evn)
			return EventResult::kContinue;

		for (RE::InputEvent* input = *evn; input != nullptr; input = input->next)
		{
			if (input->eventType.all(RE::INPUT_EVENT_TYPE::kButton))
			{
				RE::ButtonEvent* button = input->AsButtonEvent();
				if (!button)
					continue;

				using namespace InputManager;
				std::uint32_t keyCode = 0;
				std::uint32_t keyMask = button->GetIDCode();
				if (button->device.all(RE::INPUT_DEVICE::kMouse))
					keyCode = InputMap::kMacro_MouseButtonOffset + keyMask;
				else if (REL::Module::IsVR() &&
						 button->device.underlying() >= INPUT_DEVICE_CROSS_VR::kVirtualKeyboard &&
						 button->device.underlying() <= INPUT_DEVICE_CROSS_VR::kDeviceType_WindowsMRSecondary) {
					keyCode = GetDeviceOffsetForDevice(button->device.underlying()) + keyMask;
				}
				else if (button->device.all(RE::INPUT_DEVICE::kGamepad))
					keyCode = InputMap::GamepadMaskToKeycode(keyMask);
				else
					keyCode = keyMask;

				if (!REL::Module::IsVR())
				{
					if (keyCode >= InputMap::kMaxMacros)
						continue;
				}
				else
				{
					if (keyCode >= InputMap::kMaxMacrosVR)
						continue;
				}

				if (Config::GetSingleton().GetHotKey1() != 0 && keyCode == Config::GetSingleton().GetHotKey1())
				{
					isPressedHotKey1 = button->IsPressed();
				}
				else if (Config::GetSingleton().GetHotKey2() != 0 && keyCode == Config::GetSingleton().GetHotKey2())
				{
                    if (button->IsUp())
                    {
                        isResetTasks = false;
                        if (button->HeldDuration() < 3.0f && (isPressedHotKey1 || Config::GetSingleton().GetHotKey1() == 0))
						{
							RE::Actor* target = nullptr;
							if (auto consoleRef = RE::Console::GetSelectedRef(); consoleRef && Config::GetSingleton().GetUseConsoleRef())
							{
								target = skyrim_cast<RE::Actor*>(consoleRef.get());
							}
							else if (auto crossHair = RE::CrosshairPickData::GetSingleton(); crossHair && crossHair->targetActor)
							{
#ifndef ENABLE_SKYRIM_VR
								target = skyrim_cast<RE::Actor*>(crossHair->targetActor.get().get());
#else
								for (std::uint32_t i = 0; i < RE::VRControls::VR_DEVICE::kTotal; i++)
								{
									target = skyrim_cast<RE::Actor*>(crossHair->targetActor[i].get().get());
									if (target)
										break;
								}
#endif
							}
							if (!target)
								target = RE::PlayerCharacter::GetSingleton();
							if (target) geometryRetry.Forget(target->formID);
                            QUpdateNormalMap(target, BipedObjectSlot::kAll);
							std::string notification = "MDNM : Re-update ";
							notification += target->GetName();
							notification += " " + GetHexStr(target->formID);
                            RE::SendHUDMessage::ShowHUDMessage(notification.c_str());
						}
					}
                    else if (button->HeldDuration() >= 3.0f) // forced reset
                    {
                        if (isResetTasks)
                            return EventResult::kContinue;
                        if (pendingMenuApply || HasPendingUpdates() || ActorVertexHasher::GetSingleton().HasPendingDetection() ||
                            ObjectNormalMapUpdater::GetSingleton().GetCount() > 0 ||
                            NormalMapStore::GetSingleton().HasPendingDiskWrites())
                        {
                            RE::SendHUDMessage::ShowHUDMessage("MDNM : Some normalmaps are still being updated");
                            logger::warn("Some normalmaps are still being updated...");
                            return EventResult::kContinue;
                        }
                        if (!Mus::Config::GetSingleton().LoadConfig()) {
                            RE::SendHUDMessage::ShowHUDMessage("MDNM : Reload failed; check configuration");
                            logger::error("Reload failed: invalid or unreadable configuration");
                            return EventResult::kContinue;
                        }
                        NormalMapStore::GetSingleton().Init();
                        InitialSetting();
                        actorWorkState->Clear();
                        geometryRetry.Clear();
                        ConditionManager::GetSingleton().InitialConditionList();
                        static_cast<MultipleConfig*>(&Config::GetSingleton())->LoadConditionFile();
                        ConditionManager::GetSingleton().SortConditions();
                        ObjectNormalMapUpdater::GetSingleton().Init();
                        RE::SendHUDMessage::ShowHUDMessage("MDNM : Reload done");
                        logger::info("Reload done");
                        isResetTasks = true;
                    }
				}
				else if (keyCode == 29) //ctrl
				{
					if (button->IsDown())
					{
						isPressedExportHotkey1 = true;
					}
					else if (button->IsUp())
					{
						isPressedExportHotkey1 = false;
					}
				}
				else if (keyCode == 87) //F11
				{
                    if (button->IsUp() && isPressedExportHotkey1)
                    {
                        if (Config::GetSingleton().GetLogLevel() >= spdlog::level::level_enum::info)
                            continue;

                        logger::info("Print meshes...");

                        RE::Actor* target = nullptr;
                        if (auto consoleRef = RE::Console::GetSelectedRef(); consoleRef && Config::GetSingleton().GetUseConsoleRef())
                        {
                            target = skyrim_cast<RE::Actor*>(consoleRef.get());
                        }
                        if (auto crossHair = RE::CrosshairPickData::GetSingleton(); !target && crossHair && crossHair->targetActor)
                        {
#ifndef ENABLE_SKYRIM_VR
                            target = skyrim_cast<RE::Actor*>(crossHair->targetActor.get().get());
#else
                            for (std::uint32_t i = 0; i < RE::VRControls::VR_DEVICE::kTotal; i++)
                            {
                                target = skyrim_cast<RE::Actor*>(crossHair->targetActor[i].get().get());
                                if (target)
                                    break;
                            }
#endif
                        }
                        if (!target)
                            target = RE::PlayerCharacter::GetSingleton();

                        auto condition = ConditionManager::GetSingleton().GetCondition(target);
                        if (!condition.Enable)
                            continue;

                        auto gender = GetSex(target);
                        GeometryData geoData;
                        auto geometries = GetAllGeometries(target);
                        for (auto& pair : geometries)
                        {
                            auto geo = pair.second;
                            using Feature = RE::BSShaderMaterial::Feature;
                            if (!geo || geo->name.empty())
                                continue;
                            if (auto extraData = geo->GetExtraData<RE::NiIntegerExtraData>(NoDynamicNormalMapExtraDataName); extraData && extraData->value > 0)
                                continue;
                            if (!geo->GetGeometryRuntimeData().shaderProperty)
                                continue;
                            auto effect = geo->GetGeometryRuntimeData().shaderProperty.get();
                            auto lightingShader = netimmerse_cast<RE::BSLightingShaderProperty*>(effect);
                            if (!lightingShader || !lightingShader->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals))
                                continue;
                            if (auto alphaProperty = geo->GetGeometryRuntimeData().alphaProperty.get(); alphaProperty)
                                continue;
                            RE::BSLightingShaderMaterialBase* material = skyrim_cast<RE::BSLightingShaderMaterialBase*>(lightingShader->material);
                            if (!material || !material->normalTexture || material->normalTexture->name.empty())
                                continue;
                            if (!geo->GetGeometryRuntimeData().skinInstance)
                                continue;
                            bSlot slot = nif::GetBipedSlot(geo, condition.DynamicTriShapeAsHead);
                            if ((!condition.HeadEnable || !Config::GetSingleton().GetHeadEnable()) && slot == RE::BIPED_OBJECT::kHead)
                                continue;
                            geoData.CopyGeometryData(geo);
                        }
                        geoData.GetGeometryData();

                        if (geoData.vertices.empty() 
							|| geoData.indices.empty() 
							|| geoData.vertices.size() != geoData.uvs.size() 
							|| geoData.geometries.empty())
                        {
                            logger::error("{} : Invalid parameters", __func__);
                            continue;
                        }
                        geoData.GeometryProcessing();
                        std::string filePath = GetRuntimeSKSEDirectory() + "MuDynamicNormalMap\\" + GetHexStr(target->formID) + ".glb";
                        if (geoData.PrintGeometry(filePath))
                        {
                            logger::info("Print mesh done : {}", filePath);
                            RE::SendHUDMessage::ShowHUDMessage("MDNM : Print mesh done");
                        }
						else
                        {
                            logger::info("Failed to print the mesh : {}", filePath);
                            RE::SendHUDMessage::ShowHUDMessage("MDNM : Failed to print the mesh ");
						}
                    }
				}
				else if (keyCode == 88) //F12
				{
                    if (button->IsUp() && isPressedExportHotkey1)
                    {
                        if (Config::GetSingleton().GetLogLevel() >= spdlog::level::level_enum::info)
                            continue;

                        logger::info("Print textures...");
                        RE::Actor* target = nullptr;
                        if (auto consoleRef = RE::Console::GetSelectedRef(); consoleRef && Config::GetSingleton().GetUseConsoleRef())
                        {
                            target = skyrim_cast<RE::Actor*>(consoleRef.get());
                        }
                        if (auto crossHair = RE::CrosshairPickData::GetSingleton(); !target && crossHair && crossHair->targetActor)
                        {
#ifndef ENABLE_SKYRIM_VR
                            target = skyrim_cast<RE::Actor*>(crossHair->targetActor.get().get());
#else
                            for (std::uint32_t i = 0; i < RE::VRControls::VR_DEVICE::kTotal; i++)
                            {
                                target = skyrim_cast<RE::Actor*>(crossHair->targetActor[i].get().get());
                                if (target)
                                    break;
                            }
#endif
                        }
                        if (!target)
                            target = RE::PlayerCharacter::GetSingleton();

						LastNormalMapSet a_set;
                        {
                            std::shared_lock sl(lastNormalMapLock);
                            if (auto found = lastNormalMap.find(target->formID); found != lastNormalMap.end())
                                a_set = found->second;
                        }
                        bool isPrinted = false;
                        for (const auto& pair : a_set)
                        {
                            const std::string textureName = pair->GetTextureName();
                            auto texture = Shader::TextureLoadManager::GetSingleton().GetNiTexture(textureName);
                            if (!texture)
                                continue;

                            TextureInfo info;
                            if (!GetTextureInfo(textureName, info))
                                continue;

                            auto texturePath = stringRemoveEnds(info.texturePath, ".dds");
                            texturePath = texturePath + "_" + SetHex(info.actorID, false) + "_" + std::to_string(info.bipedSlot) + ".dds";
                            ;

                            if (!stringStartsWith(texturePath, "Textures\\"))
                                texturePath = GetRuntimeTexturesDirectory() + texturePath;
                            else if (!stringStartsWith(texturePath, "Data\\"))
                                texturePath = GetRuntimeDataDirectory() + texturePath;

                            if (Shader::TextureLoadManager::GetSingleton().PrintTexture(texturePath, texture.Get()))
                            {
                                logger::info("Print texture done : {}", texturePath);
                                isPrinted = true;
                            }
                        }
                        if (isPrinted)
                            RE::SendHUDMessage::ShowHUDMessage("MDNM : Print texture done");
						else
                        {
                            logger::info("Failed to print texture");
                            RE::SendHUDMessage::ShowHUDMessage("MDNM : Failed to print texture");
						}
					}
				}
			}
		}
		return EventResult::kContinue;
	}

	EventResult TaskManager::ProcessEvent(const RE::MenuOpenCloseEvent* evn, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
	{
		if (!evn || !evn->menuName.c_str())
			return EventResult::kContinue;

		if (evn->opening)
		{
			if (IsSameString(evn->menuName.c_str(), "RaceSex Menu"))
			{
                logger::info("RaceSexMenu Open");
				IsRaceSexMenu.store(true);
			}
			else if (IsSameString(evn->menuName.c_str(), "Loading Menu"))
            {
                //logger::info("Loading Menu Open");
                IsInLoading.store(true);
                if (SetImmediately(true))
                {
                    isAfterLoading = true;
                    isRevertDone = false;
                }
			}
		}
		else
		{
			if (IsSameString(evn->menuName.c_str(), "RaceSex Menu"))
            {
                logger::info("RaceSexMenu Close");
				IsRaceSexMenu.store(false);
            }
            else if (IsSameString(evn->menuName.c_str(), "Loading Menu"))
            {
                IsInLoading.store(false);
                //logger::info("Loading Menu Close");
            }
        }
        return EventResult::kContinue;
	}


	void TaskManager::ReleaseResourceOnUnloadActors()
	{
        {
            std::lock_guard lg(lastNormalMapLock);
            auto count = lastNormalMap.size();
            for (auto it = lastNormalMap.begin(); it != lastNormalMap.end();)
            {
                RE::Actor* actor = GetFormByID<RE::Actor*>(it->first);
                if (!IsInvalidActor(actor))
                {
                    it++;
                    continue;
                }
                if (GetIsUpdating(it->first))
                {
                    it++;
                    continue;
                }
                it = lastNormalMap.erase(it);
            }
            if (count != lastNormalMap.size())
                logger::debug("Current remain NiTexture {}", lastNormalMap.size());
        }
	}
}
