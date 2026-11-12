#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>

namespace Mus {
    // Defaults match Config.h at the reviewed baseline; distances are unsquared game units.
    struct RuntimeTuning {
        int detectTickMS = 1000;
        float detectDistance = 512.0f;
        float updateDistance = 4096.0f;
        int updateDelayTick = 1;
        int logLevel = 2;
        int flushLevel = 0;
        bool DebugTexture = false;
        bool TickTimeToRealFPS = false;
        bool QueueTime = false;
        bool FullUpdateTime = false;
        bool ActorVertexHasherTime1 = false;
        bool ActorVertexHasherTime2 = false;
        bool GeometryDataTime = false;
        bool UpdateNormalMapTime1 = false;
        bool UpdateNormalMapTime2 = false;
        bool MergeTime = false;
        bool GenerateMipsTime = false;
        bool BleedTextureTime1 = false;
        bool BleedTextureTime2 = false;
        bool CompressTime = false;
        bool TextureCopyTime = false;
        bool PlayerEnable = true;
        bool NPCEnable = true;
        bool MaleEnable = true;
        bool FemaleEnable = true;
        bool HeadEnable = false;
        int HotKey1 = 0;
        int HotKey2 = 43;
        bool UseConsoleRef = true;
        bool RevertNormalMap = false;
        bool ApplyOverlay = true;
        bool RemoveSkinOverrides = true;
        bool RemoveNodeOverrides = true;
        float WeldDistance = 0.02f;
        float BoundaryWeldDistance = 0.02f;
        bool WeldAccuracy = true;
        float NormalSmoothDegree = 60.0f;
        bool AllowInvertNormalSmooth = false;
        int Subdivision = 0;
        int SubdivisionTriThreshold = 65535;
        int SubdivisionVertexSmooth = 1;
        float SubdivisionVertexSmoothStrength = 0.5f;
        int VertexSmooth = 0;
        float VertexSmoothStrength = 0.5f;
        int VertexSmoothByAngle = 0;
        float VertexSmoothByAngleThreshold1 = 45.0f;
        float VertexSmoothByAngleThreshold2 = 60.0f;
        int TextureWidth = 2048;
        int TextureHeight = 2048;
        bool TangentZCorrection = true;
        float DetailStrength = 0.5f;
        bool IgnoreMissingNormalMap = true;
        bool GPUEnable = true;
        int GPUDeviceIndex = -1;
        bool GPUForceSync = false;
        bool UpdateDistanceVramSave = false;
        bool UseMipMap = true;
        int AutoTaskQ = 3;
        bool EcoreUse = false;
        bool ProcessingInLoading = false;
        bool ProcessingInLoadingWithMainGPU = true;
        int TaskQTickMS = 100;
        int TextureCompress = 1;
        int TextureCompressQuality = 1;
        int SIMDtype = 3;
        bool DiskCache = true;
        std::string DiskCacheFolder = "Data\\SKSE\\Plugins\\MuDynamicNormalMap\\DiskCache";
        int DiskCacheLimitMB = 500;
        bool ClearDiskCache = true;
        int DiskCacheHashPrecision = 9;
        bool RealtimeDetect = true;
        int RealtimeDetectHead = 1;
        bool RealtimeDetectOnBackGround = false;
        bool DetectTextureChange = true;
        bool operator==(const RuntimeTuning&) const = default;
    };

    inline bool NormalizeRuntimeTuning(const RuntimeTuning&, RuntimeTuning&, std::string&);

    class RuntimeTuningStore {
    public:
        std::shared_ptr<const RuntimeTuning> Get() const noexcept {
            return current.load(std::memory_order_acquire);
        }
        bool Apply(const RuntimeTuning& input, std::string& error) {
            RuntimeTuning normalized;
            if (!NormalizeRuntimeTuning(input, normalized, error)) return false;
            current.store(std::make_shared<const RuntimeTuning>(std::move(normalized)), std::memory_order_release);
            return true;
        }
    private:
        std::atomic<std::shared_ptr<const RuntimeTuning>> current{std::make_shared<const RuntimeTuning>()};
    };
}

// The catalog defines validation using the same member bindings used by UI and INI persistence.
#include "SettingsCatalog.h"
