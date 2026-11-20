#include "TaskManager.h"
#include "Rendering/MuBridge.h"
#include "Compat/OSkin.h"
#include "Compat/SkinPaths.h"
#include "Lifecycle.h"
#include "RestoreQueue.h"
#include "SettingsCatalog.h"

namespace Mus {
    namespace {
        // Material writes target the game's renderer even when compute uses a second GPU.
        class MaterialLock {
        public:
            MaterialLock() : renderer(RE::BSGraphics::Renderer::GetSingleton()) { renderer->Lock(); }
            ~MaterialLock() { renderer->Unlock(); }
            MaterialLock(const MaterialLock&) = delete;
            MaterialLock& operator=(const MaterialLock&) = delete;
        private:
            RE::BSGraphics::Renderer* renderer;
        };
    }
    bool TaskManager::IsActorEnabled(RE::Actor* actor) const {
        if (!actor) return false;
        const auto tuning = Config::GetSingleton().GetRuntimeTuning();
        if (IsPlayer(actor->formID) ? !tuning->PlayerEnable : !tuning->NPCEnable) return false;
        const auto sex = GetSex(actor);
        return !(sex == RE::SEX::kMale && !tuning->MaleEnable) &&
               !(sex == RE::SEX::kFemale && !tuning->FemaleEnable);
    }
    bool TaskManager::IsActorInRange(RE::Actor* actor) const {
        if (IsInvalidActor(actor)) return false;
        if (IsPlayer(actor->formID)) return true;
        const auto tuning = Config::GetSingleton().GetRuntimeTuning();
        const float distance = tuning->updateDistance * tuning->updateDistance;
        if (distance <= floatPrecision) return true;
        const auto camera = RE::PlayerCamera::GetSingleton();
        return camera && camera->GetRuntimeData2().pos.GetSquaredDistance(
            actor->loadedData->data3D->world.translate) <= distance;
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

    void TaskManager::RefreshRegisteredActors(bool discoverLoaded) {
        std::vector<RE::FormID> actors;
        {
            std::shared_lock lock(isActiveActorsLock);
            actors.reserve(isActiveActors.size());
            for (const auto& [id, active] : isActiveActors) actors.push_back(id);
        }
        // This is an explicit menu-apply pass on the main thread, never a frame scan.
        // Include actors which were already loaded before a disabled category was enabled.
        if (discoverLoaded) {
            if (auto world = RE::TES::GetSingleton()) {
                world->ForEachReference([&actors](RE::TESObjectREFR* reference) {
                    if (auto actor = skyrim_cast<RE::Actor*>(reference); !IsInvalidActor(actor))
                        actors.push_back(actor->formID);
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
            if (auto player = RE::PlayerCharacter::GetSingleton(); !IsInvalidActor(player))
                actors.push_back(player->formID);
            std::sort(actors.begin(), actors.end());
            actors.erase(std::unique(actors.begin(), actors.end()), actors.end());
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
                if (regenerate) RefreshRegisteredActors(
                    (!before->PlayerEnable && desired.PlayerEnable) || (!before->NPCEnable && desired.NPCEnable) ||
                    (!before->MaleEnable && desired.MaleEnable) || (!before->FemaleEnable && desired.FemaleEnable));
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

        // Persistent ordered registries allow bounded maintenance without copying the
        // entire population or reinserting busy/cooling requests on every frame.
        const auto distanceStart = GeometryRetry::Clock::now();
        std::size_t distanceCount;
        { std::shared_lock lock(isActiveActorsLock); distanceCount = std::min<std::size_t>(32, isActiveActors.size()); }
        for (std::size_t i = 0; i < distanceCount; ++i) {
            if (i && GeometryRetry::Clock::now() - distanceStart >= std::chrono::milliseconds(2)) break;
            RE::FormID id;
            bool wasActive;
            {
                std::unique_lock lock(isActiveActorsLock);
                if (isActiveActors.empty()) break;
                auto it = isActiveActors.upper_bound(lastDistanceActor);
                if (it == isActiveActors.end()) it = isActiveActors.begin();
                id = lastDistanceActor = it->first;
                wasActive = it->second;
            }
            auto actor = GetFormByID<RE::Actor*>(id);
            if (IsInvalidActor(actor)) {
                { std::unique_lock lock(isActiveActorsLock); isActiveActors.erase(id); }
                RemoveLastNormalMap(id);
                geometryRetry.Forget(id);
                continue;
            }
            const bool inRange = IsActorInRange(actor);
            SetIsActiveActor(id, inRange);
            // Check independently of the old active bit: enabling VRAM saving must
            // also release maps of actors that were already outside the range.
            if (!inRange && Config::GetSingleton().GetUpdateDistanceVramSave() &&
                !GetIsUpdating(id) && (IsExistLastNormalMap(id) || PNS::HasInstalledLayer(id))) RemoveNormalMap(actor);
            if (inRange && !wasActive) QUpdateNormalMap(actor);
        }

        const auto scanStart = GeometryRetry::Clock::now();
        unsigned scans = 0;
        // Alternate priority frames with an unconditional round-robin first turn.
        // Even a single slow player scan cannot starve the rest of the queue.
        const bool allowPriority = prioritizeQueueNextFrame;
        prioritizeQueueNextFrame = !prioritizeQueueNextFrame;
        std::size_t inspections = 0, inspectionLimit;
        { std::lock_guard lock(updateSlotQueueLock); inspectionLimit = std::min<std::size_t>(32, updateSlotQueue.size()); }
        for (; inspections < inspectionLimit; ++inspections) {
            if (scans >= 2 || (inspections && GeometryRetry::Clock::now() - scanStart >= std::chrono::milliseconds(2))) break;
            RE::FormID id;
            {
                std::lock_guard lock(updateSlotQueueLock);
                if (updateSlotQueue.empty()) break;
                auto it = updateSlotQueue.end();
                if (inspections == 0 && allowPriority) {
                    if (auto player = RE::PlayerCharacter::GetSingleton()) it = updateSlotQueue.find(player->formID);
                    if (it == updateSlotQueue.end() && priorityQueuedActor) it = updateSlotQueue.find(*priorityQueuedActor);
                    priorityQueuedActor.reset();
                }
                if (it == updateSlotQueue.end()) {
                    it = updateSlotQueue.upper_bound(lastScanActor);
                    if (it == updateSlotQueue.end()) it = updateSlotQueue.begin();
                    lastScanActor = it->first;
                }
                id = it->first;
            }
            auto actor = GetFormByID<RE::Actor*>(id);
            if (IsInvalidActor(actor) || !IsActorEnabled(actor)) {
                geometryRetry.Forget(id);
                std::lock_guard lock(updateSlotQueueLock);
                updateSlotQueue.erase(id);
                continue;
            }
            // Recheck actual distance at dispatch, rather than waiting for the
            // bounded distance sweep to reach a newly queued/moving actor.
            if (!IsActorInRange(actor) || GetIsUpdating(id)) continue;
            if (!ConditionManager::GetSingleton().GetCondition(actor).Enable) {
                std::lock_guard lock(updateSlotQueueLock);
                updateSlotQueue.erase(id);
                continue;
            }
            auto root = actor->loadedData->data3D.get();
            auto race = actor->GetRace();
            const auto retryState = geometryRetry.Check(id, reinterpret_cast<std::uintptr_t>(root), race ? race->formID : 0);
            if (retryState == GeometryRetry::State::CoolingDown) continue;
            bSlotbit slots;
            {
                std::lock_guard lock(updateSlotQueueLock);
                auto it = updateSlotQueue.find(id);
                if (it == updateSlotQueue.end()) continue;
                slots = it->second;
                updateSlotQueue.erase(it);
            }
            if (retryState == GeometryRetry::State::Blacklisted) continue;
            ++scans;
            auto geometries = GetAllGeometries(actor);
            if (geometries.empty()) {
                if (!geometryRetry.Failed(id, reinterpret_cast<std::uintptr_t>(root), race ? race->formID : 0)) {
                    std::lock_guard lock(updateSlotQueueLock);
                    updateSlotQueue[id] |= slots;
                }
            } else {
                geometryRetry.Forget(id);
                QUpdateNormalMapImpl(actor, std::move(geometries), slots);
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

		if (Config::GetSingleton().GetQueueTime() && inspections > 0)
            PerformanceLog(std::string("updateSlotQueue"), true, false, inspections);
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
    bool TaskManager::QUpdateNormalMap(RE::Actor* a_actor, bSlotbit bipedSlot, bool requireEnqueue)
    {
		if (stoppingComputations.load())
            return false;
        if (!a_actor || bipedSlot == 0)
            return false;
        RE::FormID id = a_actor->formID;
        std::string actorName = a_actor->GetName();
        // Remember disabled actors as well, for subsequent category enable.
        SetIsActiveActor(id, IsActorInRange(a_actor));
        if (IsPlayer(id) && !Config::GetSingleton().GetPlayerEnable())
            return !requireEnqueue;
        if (!IsPlayer(id) && !Config::GetSingleton().GetNPCEnable())
            return !requireEnqueue;
        if (GetSex(a_actor) == RE::SEX::kMale && !Config::GetSingleton().GetMaleEnable())
            return !requireEnqueue;
        if (GetSex(a_actor) == RE::SEX::kFemale && !Config::GetSingleton().GetFemaleEnable())
            return !requireEnqueue;
        if (bipedSlot & BipedObjectSlot::kHead && !Config::GetSingleton().GetHeadEnable())
            bipedSlot &= ~BipedObjectSlot::kHead;
        if (!bipedSlot)
            return !requireEnqueue;
        {
            std::lock_guard lg(updateSlotQueueLock);
            updateSlotQueue[id] |= bipedSlot;
            priorityQueuedActor = id;
        }
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
            if (PNS::ModuleEnabled() && PNS::OSkin::Active() && material->textureSet) {
                const auto* path = material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kNormal);
                if (path && *path && PNS::SkinPaths::Stale(texturePath, path))
                    texturePath = FixTexturePath(PNS::SkinPaths::Relative(path));
            }
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
                    MaterialLock materialLock;
                    if (IsExistLastNormalMap(id, newSet.slot, newSet.textureName) &&
                        !PNS::OwnsInstalledGeometry(id, geo)) {
                        // Keep a live owned layer until a replacement actually succeeds.
                        // This fast reattach also writes a material, before the async result.
                        if (PNS::ModuleEnabled()) {
                            material = PNS::PrivateNormalMaterial(id, geo);
                            if (!material) continue;
                        }
                        if (Shader::TextureLoadManager::CreateSourceTexture(newSet.textureName, material->normalTexture) >= 0)
                            PNS::RetireInstalledGeometry(id, geo);
                    }
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
        const auto session = PNS::SessionGeneration();
        auto func = [this, a_actorID, a_actorName, a_geoData, sourceRoot, session, busyTicket = std::move(busyTicket), a_updateSet = std::move(a_updateSet)]() mutable {
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

            RegisterDelayTask([this, a_actorID, a_actorName, sourceRoot, session, busyTicket = activeTicket, textures = std::move(textures)]() {
                if (stoppingComputations.load() || session != PNS::SessionGeneration() || !PNS::SessionActive()) return;
                auto actor = GetFormByID<RE::Actor*>(a_actorID);
                if (IsInvalidActor(actor))
                {
                    logger::error("{:x}::{} : invalid reference", a_actorID, a_actorName);
                    return;
                }

                // A bounded distance sweep may not have visited this actor yet.
                // Never reattach an in-flight result after it left the VRAM range.
                if (!IsActorEnabled(actor)) return;
                if (Config::GetSingleton().GetUpdateDistanceVramSave() && !IsActorInRange(actor)) {
                    // Keep a dirty request even if the bounded sweep never observed
                    // this brief exit. Dispatch will wait until the actor is in range.
                    QUpdateNormalMap(actor, BipedObjectSlot::kAll);
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
                // Equipment can replace a shape without replacing the actor's root.
                // Do not install a cached/baked old mesh via the slot/path fallback below.
                std::unordered_set<RE::BSGeometry*> liveShapes;
                RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* geometry) {
                    liveShapes.insert(geometry);
                    return RE::BSVisit::BSVisitControl::kContinue;
                });
                if (std::ranges::any_of(textures, [&](const auto& result) { return !liveShapes.contains(result.geometry); })) {
                    geometryRetry.Forget(a_actorID);
                    QUpdateNormalMap(actor, BipedObjectSlot::kAll);
                    return;
                }
                PNS::PruneInstalledLayers(a_actorID, liveShapes);
                // Serialize material installation and GPU composition with the renderer.
                MaterialLock materialLock;
                struct PreparedNormal {
                    RE::NiSourceTexturePtr texture;
                    RE::NiSourceTexturePtr restoreBase; // nonempty only for a composed pregnancy layer
                };
                std::unordered_map<std::string, PreparedNormal> createdTextures;
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
                    if (PNS::ModuleEnabled() && PNS::OSkin::Active() && material->textureSet) {
                        const auto* path = material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kNormal);
                        if (PNS::SkinPaths::Stale(found->texturePath, path ? path : ""))
                            return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    if (!found->texture || !found->texture->normalmapTexture2D || !found->texture->normalmapShaderResourceView)
                        return RE::BSVisit::BSVisitControl::kContinue;

                    RE::NiSourceTexturePtr normalmap = nullptr;
                    RE::NiSourceTexturePtr restoreBase;
                    if (auto texIt = createdTextures.find(found->textureName); texIt != createdTextures.end())
                    {
                        normalmap = texIt->second.texture;
                        restoreBase = texIt->second.restoreBase;
                    }
                    else
                    {
                        // Never mutate the shared Mu cache wrapper with an actor-specific layer.
                        auto texture = found->texture->normalmapTexture2D;
                        auto view = found->texture->normalmapShaderResourceView;
                        auto baseTexture = texture;
                        auto baseView = view;
                        if (PNS::ModuleEnabled()) {
                            const bool dinoBody = found->slot == RE::BIPED_OBJECT::kBody && PNS::HasDinoBridge();
                            const bool oskinSource = PNS::OSkin::Active() && (found->slot == RE::BIPED_OBJECT::kBody ||
                                PNS::SkinPaths::IsPackPath(found->texturePath));
                            if ((dinoBody || oskinSource) && !PNS::ReadAssignedNormal(found->texturePath, texture, view))
                                return RE::BSVisit::BSVisitControl::kContinue;
                            baseTexture = texture;
                            baseView = view;
                            if (found->slot == RE::BIPED_OBJECT::kBody) {
                                std::vector<RE::BSGeometry*> shapes;
                                for (const auto& result : textures)
                                    if (result.slot == found->slot && result.textureName == found->textureName)
                                        shapes.push_back(result.geometry);
                                PNS::ComposeMuBody(actor, shapes, texture, view, found->texturePath, dinoBody);
                            }
                        }
                        // The named Mu wrapper is mutable and shared by aliases/fast
                        // reattach. Keep it base-only; pregnancy outputs are private.
                        if (Shader::TextureLoadManager::GetSingleton().CreateNiTexture(found->textureName, baseTexture, baseView, normalmap) < 0 || !normalmap || !normalmap->rendererTexture || !normalmap->rendererTexture->texture || !normalmap->rendererTexture->resourceView)
                        {
                            logger::error("{:x}::{}::{} : Failed to create NiTexture", a_actorID, a_actorName, geo->name.c_str());
                            return RE::BSVisit::BSVisitControl::kContinue;
                        }
                        if (view.Get() != baseView.Get()) {
                            auto privateOutput = PNS::CreatePrivateNormalTexture(found->textureName, texture, view);
                            auto privateBase = PNS::CreatePrivateNormalTexture(found->textureName, baseTexture, baseView);
                            if (!privateOutput || !privateBase)
                                return RE::BSVisit::BSVisitControl::kContinue;
                            normalmap = std::move(privateOutput);
                            restoreBase = std::move(privateBase);
                        }
                        createdTextures.emplace(found->textureName, PreparedNormal{normalmap, restoreBase});
                        if (Config::GetSingleton().GetLogLevel() <= spdlog::level::level_enum::debug)
                        {
                            TextureLog(found->texture->normalmapTexture2D.Get());
                            TextureLog(found->texture->normalmapShaderResourceView.Get());
                        }
                    }

                    if (PNS::ModuleEnabled()) {
                        // A shared skin material/property must not transmit this actor's layer.
                        material = PNS::PrivateNormalMaterial(a_actorID, geo);
                        if (!material) return RE::BSVisit::BSVisitControl::kContinue;
                    }
                    // All resources are ready. Retire the previous layer only now;
                    // this also recovers the original diffuse across repeated debug bakes.
                    PNS::RetireInstalledGeometry(a_actorID, geo);
                    auto originalDiffuse = material->diffuseTexture;
                    const bool debugOverride = Config::GetSingleton().GetDebugTexture();
                    if (debugOverride)
                        material->diffuseTexture = normalmap;
                    material->normalTexture = normalmap;
                    if (restoreBase)
                        PNS::RecordInstalledLayer(actor, geo, material, normalmap.get(), restoreBase.get(),
                            debugOverride, originalDiffuse.get());
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
        if (IsInvalidActor(a_actor)) return false;
        const auto actorID = a_actor->formID;
        if (!IsExistLastNormalMap(actorID) && !PNS::HasInstalledLayer(actorID)) return true;

        MaterialLock materialLock;
        std::unordered_set<RE::BSGeometry*> restoreGeometries;
        RE::BSVisit::TraverseScenegraphGeometries(a_actor->loadedData->data3D.get(), [&](RE::BSGeometry* geometry) {
            if (geometry) restoreGeometries.insert(geometry);
            return RE::BSVisit::BSVisitControl::kContinue;
        });
        PNS::PruneInstalledLayers(actorID, restoreGeometries);
        bool restoredAll = true;
        for (auto geo : restoreGeometries)
        {
            const bool ownedLayer = PNS::OwnsInstalledGeometry(actorID, geo);
            // An external material/wrapper now owns this shape. Retire only our
            // stale bookkeeping (and a debug diffuse still demonstrably ours).
            if (!ownedLayer && PNS::RetireInstalledGeometry(actorID, geo)) continue;
            auto* lightingShader = netimmerse_cast<RE::BSLightingShaderProperty*>(
                geo->GetGeometryRuntimeData().shaderProperty.get());
            auto* material = lightingShader ? skyrim_cast<RE::BSLightingShaderMaterialBase*>(lightingShader->material) : nullptr;
            if (!lightingShader || !material || !material->normalTexture ||
                !lightingShader->flags.all(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals)) {
                if (ownedLayer) restoredAll = false;
                continue;
            }
            // Include generated overlays, but leave unrelated mods' replacements alone.
            if (material->normalTexture->name.empty() || !IsCreatedByMDNM(material->normalTexture->name.c_str()))
                continue;
            // A retired private layer may retain its Mu name after an external
            // replacement. Only a registered base or a live owned layer can be reset.
            if (!ownedLayer && !Shader::TextureLoadManager::GetSingleton().OwnsNamedTexture(material->normalTexture.get()))
                continue;
            if (!material->textureSet) {
                restoredAll = false;
                continue;
            }
            std::string texturePath = lowLetter(GetTexturePath(material->textureSet.get(), RE::BSTextureSet::Texture::kNormal));
            if (texturePath.empty()) {
                restoredAll = false;
                continue;
            }
            RE::NiPointer<RE::NiSourceTexture> texture;
            Shader::TextureLoadManager::GetSingleton().LoadTexture(texturePath.c_str(), 1, texture, false);
            if (!texture) {
                restoredAll = false;
                continue;
            }
            material->normalTexture = texture;
            PNS::RetireInstalledGeometry(actorID, geo);
        }
        // The distance sweep uses this registration to retry failed restores.
        // Successful shapes have already released their pregnancy resource pins.
        if (restoredAll) RemoveLastNormalMap(actorID);
        return restoredAll;
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

unsigned PNS::MuUpdateDelayFrames() { return Mus::Config::GetSingleton().GetUpdateDelayTick(); }
bool PNS::QueueMuSlots(RE::Actor* actor, std::uint32_t slots) {
    if (!ModuleEnabled() || !SessionActive()) return false;
    return Mus::TaskManager::GetSingleton().QUpdateNormalMap(actor, slots, true);
}
bool PNS::QueueMuBody(RE::Actor* actor) { return QueueMuSlots(actor, 1u << RE::BIPED_OBJECT::kBody); }

bool PNS::QueueLayerRestore(std::function<void()> restore) {
    if (!ModuleEnabled() || !SessionActive() || !restore) return false;
    Mus::TaskManager::GetSingleton().RegisterDelayTask(std::move(restore));
    return true;
}

RE::NiSourceTexturePtr PNS::ResolveRestoredBase(RE::NiSourceTexture* savedBase) {
    if (!savedBase || savedBase->name.empty() || !savedBase->rendererTexture) return {};
    const auto name = savedBase->name.c_str();
    auto& textures = Mus::Shader::TextureLoadManager::GetSingleton();
    RE::NiSourceTexturePtr base;
    if (textures.CreateSourceTexture(name, base) < 0 || !base) return {};
    if (base->rendererTexture && base->rendererTexture->texture && base->rendererTexture->resourceView) return base;
    // An evicted base can be rebuilt from the saved resources. Use the named
    // base so Mu can still release it when the actor moves out of range.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture =
        reinterpret_cast<ID3D11Texture2D*>(savedBase->rendererTexture->texture);
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view =
        reinterpret_cast<ID3D11ShaderResourceView*>(savedBase->rendererTexture->resourceView);
    if (textures.CreateNiTexture(name, texture, view, base) < 0) return {};
    return base;
}

RE::NiSourceTexturePtr PNS::CreatePrivateNormalTexture(const std::string& sourceName,
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view) {
    if (!texture || !view) return {};
    // CreateTexture allocates the engine object; CreateSourceTexture would also
    // register/reuse it in Mu's mutable name cache, which is deliberately bypassed.
    static std::uint64_t sequence = 0; // game thread only
    const auto allocationName = sourceName + "::PNSPrivate::" + std::to_string(++sequence);
    RE::NiSourceTexturePtr result{static_cast<RE::NiSourceTexture*>(
        Mus::Shader::TextureLoadManager::CreateTexture(allocationName.c_str()))};
    if (!result || result->rendererTexture) return {};
    auto renderer = std::make_unique<RE::BSGraphics::Texture>();
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    renderer->width = static_cast<std::uint16_t>(desc.Width);
    renderer->height = static_cast<std::uint16_t>(desc.Height);
    renderer->mips = static_cast<std::uint8_t>(desc.MipLevels);
    renderer->format = static_cast<std::uint8_t>(desc.Format);
    renderer->texture = reinterpret_cast<REX::W32::ID3D11Texture2D*>(texture.Detach());
    renderer->resourceView = reinterpret_cast<REX::W32::ID3D11ShaderResourceView*>(view.Detach());
    result->rendererTexture = renderer.release();
    // Preserve the canonical metadata consumed by Mu and Dino path parsers.
    Mus::nif::setBSFixedString(result->name, sourceName.c_str());
    return result;
}
