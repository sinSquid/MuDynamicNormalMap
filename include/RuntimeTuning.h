#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>

namespace Mus {
    struct RuntimeTuning {
        int detectTickMS = 1000;
        float detectDistance = 512.0f;
        float updateDistance = 4096.0f;
        int updateDelayTick = 1;
        bool operator==(const RuntimeTuning&) const = default;
    };

    inline bool NormalizeRuntimeTuning(const RuntimeTuning& input, RuntimeTuning& normalized, std::string& error) {
        if (input.detectTickMS < 0 || input.detectTickMS > 60000) {
            error = "Detection interval must be between 0 and 60000 ms.";
            return false;
        }
        if (input.updateDelayTick < 0 || input.updateDelayTick > 255) {
            error = "Mesh delay must be between 0 and 255 frames.";
            return false;
        }
        if (!std::isfinite(input.detectDistance) || !std::isfinite(input.updateDistance) ||
            input.detectDistance < 0 || input.updateDistance < 0 ||
            input.detectDistance > 100000.0f || input.updateDistance > 100000.0f) {
            error = "Distances must be finite values between 0 and 100000.";
            return false;
        }
        normalized = input;
        // The existing distance tests use squared game units and 1e-6 precision.
        if (normalized.detectDistance <= 0.001f) normalized.detectDistance = 0;
        if (normalized.updateDistance <= 0.001f) normalized.updateDistance = 0;
        if (normalized.detectDistance == 0 || normalized.updateDistance == 0)
            normalized.updateDistance = 0;
        else
            normalized.updateDistance = std::max(normalized.updateDistance, normalized.detectDistance);
        error.clear();
        return true;
    }

    class RuntimeTuningStore {
    public:
        std::shared_ptr<const RuntimeTuning> Get() const noexcept {
            return current.load(std::memory_order_acquire);
        }
        bool Apply(const RuntimeTuning& input, std::string& error) {
            RuntimeTuning normalized;
            if (!NormalizeRuntimeTuning(input, normalized, error)) return false;
            auto next = std::make_shared<const RuntimeTuning>(normalized);
            current.store(std::move(next), std::memory_order_release);
            return true;
        }
    private:
        std::atomic<std::shared_ptr<const RuntimeTuning>> current{std::make_shared<const RuntimeTuning>()};
    };
}
