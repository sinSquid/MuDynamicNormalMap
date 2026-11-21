#include <algorithm>
#include <cmath>
#include <sstream>
#include "../include/SettingsCatalog.h"
#include "../include/NormalMath.h"
#include <cassert>

int main() {
    Mus::RuntimeTuning gpu, cpu;
    gpu.GPUEnable = true;
    cpu = gpu;
    cpu.GPUEnable = false;
    assert(Mus::RuntimeGenerationSignature(gpu) != Mus::RuntimeGenerationSignature(cpu));
    assert(Mus::RuntimeGenerationSignature(gpu, false) == Mus::RuntimeGenerationSignature(cpu, false));
    assert(Mus::RuntimeGenerationSignature(cpu, true) == Mus::RuntimeGenerationSignature(gpu, true));
    auto pacing = gpu;
    pacing.TaskQTickMS += 1;
    pacing.detectTickMS += 1;
    assert(Mus::RuntimeGenerationSignature(pacing) == Mus::RuntimeGenerationSignature(gpu));
    auto changedPixels = gpu;
    changedPixels.DetailStrength += 0.1f;
    assert(Mus::RuntimeGenerationSignature(changedPixels) != Mus::RuntimeGenerationSignature(gpu));
    // Restart-only scheduling/device-selection settings do not change the
    // selected generation algorithm or its inputs.
    auto restarted = gpu;
    restarted.GPUDeviceIndex = 0;
    restarted.ProcessingInLoading = !gpu.ProcessingInLoading;
    restarted.RealtimeDetectOnBackGround = !gpu.RealtimeDetectOnBackGround;
    assert(Mus::RuntimeGenerationSignature(restarted) == Mus::RuntimeGenerationSignature(gpu));

    const float strength = 0.5f;
    const float adjacent = std::nextafter(strength, 1.0f);
    assert(std::to_string(strength) == std::to_string(adjacent)); // Previous override-key collision.
    assert(Mus::FloatValueText(strength) != Mus::FloatValueText(adjacent));
    auto preciseSetting = gpu;
    preciseSetting.DetailStrength = adjacent;
    assert(Mus::RuntimeGenerationSignature(preciseSetting) != Mus::RuntimeGenerationSignature(gpu));
    for (float value : {0.0f, adjacent, 0.000000123456f, 0.99999994f, 123456.75f}) {
        const auto text = Mus::FloatValueText(value);
        float parsed = 0;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
        assert(result.ec == std::errc{} && result.ptr == text.data() + text.size() && parsed == value);
    }

    // A valid varying normal field demonstrates why backend identity matters.
    // Match the existing CPU two-stage slerp and GPU normalized barycentric sum.
    using namespace DirectX;
    const auto n0 = XMVectorSet(1, 0, 0, 0);
    const auto n1 = XMVectorSet(0, 1, 0, 0);
    const auto n2 = XMVectorSet(0, 0, 1, 0);
    const auto cpuNormal = Mus::SlerpUnitVector(Mus::SlerpUnitVector(n0, n1, 0.3f / (0.2f + 0.3f + 1e-6f)), n2, 0.5f);
    const auto gpuNormal = Mus::NormalizeOr(XMVectorSet(0.2f, 0.3f, 0.5f, 0), n2);
    assert(XMVectorGetX(XMVector3Length(XMVectorSubtract(cpuNormal, gpuNormal))) > 0.05f);
}
