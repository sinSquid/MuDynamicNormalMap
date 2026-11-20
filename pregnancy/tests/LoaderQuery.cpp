#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>

struct QueryInput {
    std::uint32_t skse, runtime, editorVersion, isEditor;
};
struct PluginInfo {
    std::uint32_t abi;
    const char* name;
    std::uint32_t version;
};

int wmain(int argc, wchar_t** argv) {
    if (argc != 4 && argc != 5) return 1;
    auto module = LoadLibraryW(argv[1]);
    if (!module) {
        std::fprintf(stderr, "LoadLibrary failed (%lu)\n", GetLastError());
        return 2;
    }
    using Query = bool (*)(QueryInput*, PluginInfo*);
    auto query = reinterpret_cast<Query>(GetProcAddress(module, "SKSEPlugin_Query"));
    if (!query || !GetProcAddress(module, "SKSEPlugin_Load") || !GetProcAddress(module, "SKSEPlugin_Version")) return 3;
    QueryInput input{wcstoul(argv[2], nullptr, 16), wcstoul(argv[3], nullptr, 16), 0, 0};
    PluginInfo info{};
    if (!query(&input, &info) || info.abi != 1 || !info.name ||
        std::strcmp(info.name, "MuDynamicNormalMap") != 0 || info.version != 0x03000002) return 4;
    using LayerKey = std::uint64_t (*)(std::uint32_t);
    auto key = reinterpret_cast<LayerKey>(GetProcAddress(module, "PNS_DinoLayerKeyV1"));
    if (!key || key(0) != 0 || !GetProcAddress(module, "PNS_DinoComposeV1")) return 5;
    if (argc == 5) {
        auto dino = LoadLibraryW(argv[4]);
        if (!dino || !GetProcAddress(dino, "SKSEPlugin_Load") || !GetProcAddress(dino, "DinoRMF_PregnancyBridgeV1")) return 6;
        auto dinoQuery = reinterpret_cast<Query>(GetProcAddress(dino, "SKSEPlugin_Query"));
        if (!dinoQuery || !dinoQuery(&input, &info) || info.abi != 1 || !info.name ||
            std::strcmp(info.name, "DinoRMF") != 0 || info.version != 0x01040010) return 7;
    }
    std::printf("Windows DLL load and legacy query passed for runtime 0x%08X\n", input.runtime);
    return 0;
}

