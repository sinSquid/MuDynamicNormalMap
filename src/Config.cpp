#include "Config.h"
#include "RuntimeSettingsFiles.h"

namespace Mus {
    bool Config::LoadLogging()
    {
        std::string configPath = GetRuntimeSKSEDirectory();
        configPath += SKSE::PluginDeclaration::GetSingleton()->GetName().data();
        configPath += ".ini";

        std::ifstream file(configPath);

        if (!file.is_open())
        {
            std::transform(configPath.begin(), configPath.end(), configPath.begin(), ::tolower);
            file.open(configPath);
        }

        if (!file.is_open())
        {
            return false;
        }

        RuntimeTuning values;
        std::string error;
        const auto folder = std::filesystem::path(GetRuntimeSKSEDirectory());
        if (!LoadRuntimeSettingsFiles(folder / "MuDynamicNormalMap.ini", folder / "MuDynamicNormalMap_User.ini",
            *GetRuntimeTuning(), values, error, true)) return false;
        return ApplyRuntimeTuning(values, error);
    }

    bool Config::LoadConfig() {
        std::string configPath = GetRuntimeSKSEDirectory();
        configPath += SKSE::PluginDeclaration::GetSingleton()->GetName().data();
        configPath += ".ini";

        std::ifstream file(configPath);

        if (!file.is_open())
        {
			lowLetter(configPath);

            file.open(configPath);
            if (!file.is_open())
            {
                logger::critical("Unable to load Config file.");
                return false;
            }
        }

        RuntimeTuning values;
        std::string error;
        const auto folder = std::filesystem::path(GetRuntimeSKSEDirectory());
        if (!LoadRuntimeSettingsFiles(folder / "MuDynamicNormalMap.ini", folder / "MuDynamicNormalMap_User.ini",
            *GetRuntimeTuning(), values, error) || !ApplyRuntimeTuning(values, error)) {
            logger::error("Unable to load default/user configuration: {}", error);
            return false;
        }
        return true;
    }

    bool Config::LoadConfig(std::ifstream& configfile)
    {
        if (!configfile.is_open() || !configfile.good()) return false;
        RuntimeTuning values;
        std::string error;
        if (!ParseRuntimeTuningIni(configfile, *GetRuntimeTuning(), values, error) ||
            !ApplyRuntimeTuning(values, error)) {
            logger::error("Unable to load configuration: {}", error);
            return false;
        }
        return true;
    }

    bool MultipleConfig::LoadConditionFile()
    {
        std::string conditionPath = GetRuntimeSKSEDirectory();
        conditionPath += SKSE::PluginDeclaration::GetSingleton()->GetName().data();
        auto files = GetAllFiles(conditionPath);
        const auto size = files.size();
        tbb::parallel_for(
            tbb::blocked_range<std::size_t>(0, size),
            [&](const tbb::blocked_range<std::size_t>& r) {
                for (std::size_t i = r.begin(); i != r.end(); ++i)
                {
                    auto& file = files[i];
                    std::u8string filename_utf8 = file.filename().u8string();
                    std::string filename(filename_utf8.begin(), filename_utf8.end());
                    if (filename == "." || filename == "..")
                        continue;
                    if (!stringEndsWith(filename, ".ini"))
                        continue;
                    std::ifstream ifile(file);
                    if (!ifile.is_open())
                        continue;

                    logger::info("File found: {}", filename);

                    ConditionManager::Condition condition;
                    condition.fileName = filename;
                    condition.HeadEnable = GetHeadEnable();
                    condition.DetailStrength = GetDetailStrength();

                    bool isNormalConditionFile = false;
                    bool isConditionState = false;

                    std::string line;
                    while (std::getline(ifile, line))
                    {
                        skipComments(line);
                        trim(line);
                        if (line.length() > 0)
                        {
                            std::string variableName;
                            std::string variableValue = GetConfigSetting(line, variableName);
                            isConditionState = variableName == "Condition";

                            if (variableName == "Enable")
                            {
                                condition.Enable = GetBoolValue(variableValue);
                                isNormalConditionFile = true;
                            }
                            else if (variableName == "HeadEnable")
                            {
                                condition.HeadEnable = GetBoolValue(variableValue);
                                isNormalConditionFile = true;
                            }
                            else if (variableName == "DynamicTriShapeAsHead")
                            {
                                condition.DynamicTriShapeAsHead = GetBoolValue(variableValue);
                                isNormalConditionFile = true;
                            }
                            else if (variableName == "DetailStrength")
                            {
                                condition.DetailStrength = GetFloatValue(variableValue);
                                isNormalConditionFile = true;
                            }
                            else if (variableName == "ProxyDetailTextureFolder" || variableName == "ProxyTangentTextureFolder")
                            {
                                if (!variableValue.empty())
                                {
                                    for (auto& value : split(variableValue, ','))
                                    {
                                        condition.ProxyDetailTextureFolder.push_back(value);
                                    }
                                }
                                isNormalConditionFile = true;
                            }
                            else if (variableName == "ProxyOverlayTextureFolder")
                            {
                                if (!variableValue.empty())
                                {
                                    for (auto& value : split(variableValue, ','))
                                    {
                                        condition.ProxyOverlayTextureFolder.push_back(value);
                                    }
                                }
                                isNormalConditionFile = true;
                            }
                            else if (variableName == "ProxyMaskTextureFolder")
                            {
                                if (!variableValue.empty())
                                {
                                    for (auto& value : split(variableValue, ','))
                                    {
                                        condition.ProxyMaskTextureFolder.push_back(value);
                                    }
                                }
                                isNormalConditionFile = true;
                            }
                            else if (variableName == "ProxyFirstScan")
                            {
                                condition.ProxyFirstScan = GetBoolValue(variableValue);
                            }
                            else if (variableName == "Priority")
                            {
                                condition.Priority = GetIntValue(variableValue);
                                isNormalConditionFile = true;
                            }
                            else if (variableName == "Condition")
                            {
                                condition.originalCondition = variableValue;
                                isNormalConditionFile = true;
                            }
                            else
                            {
                                if (isConditionState)
                                    condition.originalCondition += " " + variableValue;
                            }
                        }
                    }
                    if (isNormalConditionFile)
                        ConditionManager::GetSingleton().RegisterCondition(condition);
                }
            },
            tbb::auto_partitioner()
        );
        return true;
    }

    SIMDType GetSIMDType(bool scan)
    {
        static SIMDType SIMDType_ = SIMDType::noSIMD;
        if (SIMDType_ != SIMDType::noSIMD && !scan)
            return SIMDType_;

        SIMDType_ = SIMDType::noSIMD;
        int info[4] = {};
        auto configSIMD = Config::GetSingleton().GetSIMDtype();
        if (configSIMD <= 0)
            configSIMD = SIMDType::avx2;
        if (configSIMD >= SIMDType::avx2)
        {
            __cpuid(info, 0);
            if (info[0] >= 7)
            {
                __cpuidex(info, 7, 0);
                if ((info[1] & (1 << 5)) != 0)
                    SIMDType_ = SIMDType::avx2;
            }
        }
        if (SIMDType_ == SIMDType::noSIMD)
        {
            __cpuid(info, 1);
            if ((info[2] & (1 << 28)) != 0 && configSIMD >= SIMDType::avx)
                SIMDType_ = SIMDType::avx;
            else if ((info[2] & (1 << 19)) != 0 && configSIMD >= SIMDType::sse4)
                SIMDType_ = SIMDType::sse4;
            else
                SIMDType_ = SIMDType::sse2;
        }
        logger::info("Set SIMD type : {}", magic_enum::enum_name(SIMDType_).data());
        return SIMDType_;
    }

    void InitialSetting()
    {
        Shader::ShaderManager::GetSingleton().SetSearchSecondGPU(Config::GetSingleton().GetGPUDeviceIndex() != 0);
        ObjectNormalMapUpdater::GetSingleton().ClearGeometryResourceData();

        isSecondGPUEnabled = Config::GetSingleton().GetGPUDeviceIndex() != 0 && Shader::ShaderManager::GetSingleton().IsValidSecondGPU();

        const std::uint32_t totalCoreCount = std::thread::hardware_concurrency();
        auto getPCores = [](std::uint32_t& cores, std::uint64_t& coreMask) {
            cores = 0;
            coreMask = 0;
            DWORD len = 0;
            GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
            std::vector<BYTE> buffer(len);
            if (GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &len))
            {
                BYTE* ptr = buffer.data();
                BYTE maxEfficiency = 0;
                while (ptr < buffer.data() + len)
                {
                    auto info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
                    if (info->Relationship == RelationProcessorCore)
                    {
                        if (info->Processor.EfficiencyClass > maxEfficiency)
                        {
                            maxEfficiency = info->Processor.EfficiencyClass;
                        }
                    }
                    ptr += info->Size;
                }
                ptr = buffer.data();
                while (ptr < buffer.data() + len)
                {
                    auto info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
                    if (info->Relationship == RelationProcessorCore)
                    {
                        if (info->Processor.EfficiencyClass == maxEfficiency)
                        {
                            coreMask |= info->Processor.GroupMask[0].Mask;
                        }
                    }
                    ptr += info->Size;
                }
            }
            cores = std::popcount(coreMask);
        };
        auto getECores = [totalCoreCount](std::uint32_t& cores, std::uint64_t& coreMask) {
            cores = 0;
            coreMask = 0;
            DWORD len = 0;
            GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
            std::vector<BYTE> buffer(len);
            if (GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &len))
            {
                BYTE* ptr = buffer.data();
                while (ptr < buffer.data() + len)
                {
                    auto info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
                    if (info->Relationship == RelationProcessorCore)
                    {
                        if (info->Processor.EfficiencyClass == 0)
                        {
                            coreMask |= info->Processor.GroupMask[0].Mask;
                        }
                    }
                    ptr += info->Size;
                }
            }
            cores = std::popcount(coreMask);
            if (coreMask == 0 || cores == 0 || cores == totalCoreCount)
            {
                coreMask = 0;
                cores = 0;
            }
        };

        std::uint32_t actorThreadCount = 1;
        std::uint32_t memoryManageThreadCount = 1;
        std::uint32_t processingThreadCount = totalCoreCount / 2;
        std::uint32_t gpuTaskThreadCount = 1;
        std::uint32_t cores = 0;
        std::uint64_t coreMask = 0;
        isNoSplitGPU = false;
        isImmediately = false;
        std::clock_t taskQTickMS = 1000.0f;
        bool directTaskQ = false;
        bool usePCores = false;
        bool useECores = false;
        switch (Config::GetSingleton().GetAutoTaskQ())
        {
        case Config::AutoTaskQList::Immediately:
            taskQTickMS = 0;
            directTaskQ = true;
            divideTaskQ = 0;

            isNoSplitGPU = true;
            isImmediately = true;

            waitSleepTime = std::chrono::microseconds(100);

            actorThreadCount = 4;
            getPCores(cores, coreMask);
            processingThreadCount = cores;
            usePCores = true;
            useECores = false;
            gpuTaskThreadCount = 4;
            break;
        case Config::AutoTaskQList::Fastest:
            taskQTickMS = 0;
            directTaskQ = true;
            divideTaskQ = 0;

            isNoSplitGPU = true;
            isImmediately = false;

            waitSleepTime = std::chrono::microseconds(100);

            actorThreadCount = 1;
            usePCores = !Config::GetSingleton().GetEcoreUse();
            useECores = Config::GetSingleton().GetEcoreUse();
            if (useECores)
                getECores(cores, coreMask);
            if (cores == 0)
            {
                getPCores(cores, coreMask);
                usePCores = true;
                useECores = false;
            }
            processingThreadCount = cores;
            gpuTaskThreadCount = 1;
            break;
        case Config::AutoTaskQList::Faster:
            taskQTickMS = 0;
            directTaskQ = true;
            divideTaskQ = 0;

            isNoSplitGPU = true;
            isImmediately = false;

            waitSleepTime = std::chrono::microseconds(100);

            actorThreadCount = 1;
            usePCores = !Config::GetSingleton().GetEcoreUse();
            useECores = Config::GetSingleton().GetEcoreUse();
            if (useECores)
                getECores(cores, coreMask);
            if (cores == 0)
            {
                getPCores(cores, coreMask);
                usePCores = true;
                useECores = false;
            }
            processingThreadCount = std::max(4u, cores / 2);
            gpuTaskThreadCount = 1;
            break;
        default:
        case Config::AutoTaskQList::Balanced:
            taskQTickMS = Config::GetSingleton().GetTaskQTickMS();
            directTaskQ = true;
            divideTaskQ = 0;

            isNoSplitGPU = false;
            isImmediately = false;

            waitSleepTime = std::chrono::microseconds(1000);

            actorThreadCount = 1;
            usePCores = !Config::GetSingleton().GetEcoreUse();
            useECores = Config::GetSingleton().GetEcoreUse();
            if (useECores)
                getECores(cores, coreMask);
            if (cores == 0)
            {
                getPCores(cores, coreMask);
                usePCores = true;
                useECores = false;
            }
            processingThreadCount = std::max(4u, cores / 2);
            gpuTaskThreadCount = 1;
            break;
        case Config::AutoTaskQList::BetterPerformance:
            taskQTickMS = Config::GetSingleton().GetTaskQTickMS();
            directTaskQ = false;
            divideTaskQ = 0;

            isNoSplitGPU = false;
            isImmediately = false;

            waitSleepTime = std::chrono::microseconds(1000);

            actorThreadCount = 1;
            usePCores = !Config::GetSingleton().GetEcoreUse();
            useECores = Config::GetSingleton().GetEcoreUse();
            if (useECores)
                getECores(cores, coreMask);
            if (cores == 0)
            {
                getPCores(cores, coreMask);
                usePCores = true;
                useECores = false;
            }
            processingThreadCount = std::max(2u, std::min(4u, cores / 2));
            gpuTaskThreadCount = 1;
            break;
        case Config::AutoTaskQList::BestPerformance:
            taskQTickMS = Config::GetSingleton().GetTaskQTickMS();
            directTaskQ = false;
            divideTaskQ = 1;

            isNoSplitGPU = false;
            isImmediately = false;

            waitSleepTime = std::chrono::microseconds(1000);

            actorThreadCount = 1;
            usePCores = !Config::GetSingleton().GetEcoreUse();
            useECores = Config::GetSingleton().GetEcoreUse();
            if (useECores)
                getECores(cores, coreMask);
            if (cores == 0)
            {
                getPCores(cores, coreMask);
                usePCores = true;
                useECores = false;
            }
            processingThreadCount = 2;
            gpuTaskThreadCount = 1;
            break;
        }

        std::string whatCores = "all Cores";
        if (usePCores)
            whatCores = "all P-Cores";
        else if (useECores)
            whatCores = "all E-Cores";
        gpuTask = std::make_unique<ThreadPool_GPUTaskModule>(gpuTaskThreadCount, coreMask, taskQTickMS, directTaskQ);

        actorThreads = std::make_shared<ThreadPool_ParallelModule>(actorThreadCount, coreMask);
        actorThreadsFull = std::make_shared<ThreadPool_ParallelModule>(4, coreMask);
        logger::info("set actorThreads {} on {}", actorThreadCount, whatCores);
        currentActorThreads = actorThreads;

        processingThreads = std::make_shared<TBB_ThreadPool>(processingThreadCount, coreMask);
        processingThreadsFull = std::make_shared<TBB_ThreadPool>(cores, coreMask);
        logger::info("set processingThreads {} on {}", processingThreadCount, whatCores);
        currentProcessingThreads = processingThreads;

        backGroundWorkerThreads = std::make_unique<ThreadPool_ParallelModule>(1u, coreMask);

        weldDistance = std::max(floatPrecision, Config::GetSingleton().GetWeldDistance());
        weldDistanceMult = 1.0f / weldDistance;

        boundaryWeldDistance = std::max(floatPrecision, Config::GetSingleton().GetBoundaryWeldDistance());
        boundaryWeldDistanceMult = 1.0f / boundaryWeldDistance;

        switch (GetSIMDType(true))
        {
        case SIMDType::avx2:
            ispc::bc7e_avx2_compress_block_init();
            break;
        case SIMDType::avx:
            ispc::bc7e_avx_compress_block_init();
            break;
        case SIMDType::sse4:
            ispc::bc7e_sse4_compress_block_init();
            break;
        case SIMDType::sse2:
            ispc::bc7e_sse2_compress_block_init();
            break;
        }

        if (Config::GetSingleton().GetRealtimeDetectOnBackGround())
        {
            g_armorAttachEventEventDispatcher.addListener(&ActorVertexHasher::GetSingleton());
            g_facegenNiNodeEventDispatcher.addListener(&ActorVertexHasher::GetSingleton());
            g_actorChangeHeadPartEventDispatcher.addListener(&ActorVertexHasher::GetSingleton());
            if (const auto NiNodeEvent = SKSE::GetNiNodeUpdateEventSource(); NiNodeEvent)
                NiNodeEvent->AddEventSink<SKSE::NiNodeUpdateEvent>(&Mus::ActorVertexHasher::GetSingleton());
        }
        else
        {
            g_armorAttachEventEventDispatcher.removeListener(&Mus::ActorVertexHasher::GetSingleton());
            g_facegenNiNodeEventDispatcher.removeListener(&Mus::ActorVertexHasher::GetSingleton());
            g_actorChangeHeadPartEventDispatcher.removeListener(&ActorVertexHasher::GetSingleton());
            if (const auto NiNodeEvent = SKSE::GetNiNodeUpdateEventSource(); NiNodeEvent)
                NiNodeEvent->RemoveEventSink(&Mus::ActorVertexHasher::GetSingleton());
        }
    }
    bool SetImmediately(bool a_Immediately)
    {
        if (!Config::GetSingleton().GetProcessingInLoading())
            return true;
        static std::atomic<bool> isImmediately_ = isImmediately.load();
        static std::atomic<bool> isNoSplitGPU_ = isNoSplitGPU.load();
        static std::atomic<bool> isSecondGPUEnabled_ = isSecondGPUEnabled.load();
        if (a_Immediately)
        {
            if (ObjectNormalMapUpdater::GetSingleton().GetCount() > 0)
            {
                logger::warn("Some normalmaps are still being updated... so GPU cannot be switched");
                return false;
            }
            else if (Config::GetSingleton().GetProcessingInLoadingWithMainGPU())
            {
                isImmediately_ = isImmediately.load();
                isImmediately = a_Immediately;
                isSecondGPUEnabled_ = isSecondGPUEnabled.load();
                isSecondGPUEnabled = !a_Immediately;
            }
        }
        else
        {
            if (ObjectNormalMapUpdater::GetSingleton().GetCount() > 0)
            {
                logger::warn("Some normalmaps are still being updated... so GPU cannot be switched");
                return false;
            }
            else if (Config::GetSingleton().GetProcessingInLoadingWithMainGPU())
            {
                isImmediately = isImmediately_.load();
                isSecondGPUEnabled = isSecondGPUEnabled_.load();
            }
        }
        if (isImmediately != isImmediately_)
        {
            currentProcessingThreads = processingThreadsFull;
            currentActorThreads = actorThreadsFull;
            isNoSplitGPU_ = isNoSplitGPU.load();
            isNoSplitGPU = true;
        }
        else
        {
            currentProcessingThreads = processingThreads;
            currentActorThreads = actorThreads;
            isNoSplitGPU = isNoSplitGPU_.load();
        }
        return true;
    }
}
