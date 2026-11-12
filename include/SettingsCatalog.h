#pragma once
#include "RuntimeTuning.h"
#include <charconv>
#include <cctype>
#include <limits>
#include <istream>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace Mus {
    enum class SettingType { Boolean, Integer, Float, String, Choice };
    enum class ApplyPolicy { Immediate, IdleReconfigure, Regenerate, Restart };
    using SettingValue = std::variant<bool, int, float, std::string>;

    struct SettingDescriptor {
        const char* section;
        const char* key;
        const char* label;
        const char* help;
        SettingType type;
        // Menu recommendations may be narrower than the accepted INI/type range.
        double minimum;
        double maximum;
        const char* options; // Pipe-separated labels, starting at minimum.
        ApplyPolicy policy;
        bool advanced;
        SettingValue (*read)(const RuntimeTuning&);
        bool (*assign)(RuntimeTuning&, const SettingValue&);
        double hardMinimum;
        double hardMaximum;
    };

    template<auto Member>
    inline SettingDescriptor MakeSetting(const char* section, const char* key, const char* label,
        const char* help, SettingType type, double minimum, double maximum, const char* options,
        ApplyPolicy policy, bool advanced, double hardMinimum, double hardMaximum) {
        using T = std::remove_cvref_t<decltype(std::declval<RuntimeTuning>().*Member)>;
        return {section, key, label, help, type, minimum, maximum, options, policy, advanced,
            +[](const RuntimeTuning& values) -> SettingValue { return values.*Member; },
            +[](RuntimeTuning& values, const SettingValue& input) {
                if (const auto* value = std::get_if<T>(&input)) { values.*Member = *value; return true; }
                return false;
            }, hardMinimum, hardMaximum};
    }

    // Author rules: upstream 2ae3 Config.cpp and shipped INI comments.
    // Heavy subdivision/smoothing slider limits are menu recommendations, not author limits.
    inline const std::vector<SettingDescriptor>& GetSettingsCatalog() {
        static const std::vector<SettingDescriptor> catalog = {
            MakeSetting<&RuntimeTuning::PlayerEnable>("General", "PlayerEnable", "启用玩家", "恢复已有结果并重新生成；关闭的角色类别不再更新。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::NPCEnable>("General", "NPCEnable", "启用NPC", "恢复已有结果并重新生成；关闭的角色类别不再更新。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::MaleEnable>("General", "MaleEnable", "启用男性", "恢复已有结果并重新生成；关闭的角色类别不再更新。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::FemaleEnable>("General", "FemaleEnable", "启用女性", "恢复已有结果并重新生成；关闭的角色类别不再更新。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::HeadEnable>("General", "HeadEnable", "启用头部", "恢复已有结果并重新生成；关闭的角色类别不再更新。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::HotKey1>("General", "HotKey1", "更新快捷键修饰键", "DX扫描码：键盘、鼠标和手柄为0–281；0表示无修饰键。", SettingType::Integer, 0, 281, "", ApplyPolicy::Immediate, false, 0, 281),
            MakeSetting<&RuntimeTuning::HotKey2>("General", "HotKey2", "更新快捷键主键", "DX扫描码：键盘、鼠标和手柄为0–281；0表示禁用此快捷键。", SettingType::Integer, 0, 281, "", ApplyPolicy::Immediate, false, 0, 281),
            MakeSetting<&RuntimeTuning::UseConsoleRef>("General", "UseConsoleRef", "优先使用控制台选中对象", "立即应用于后续操作。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, false, 0, 1),
            MakeSetting<&RuntimeTuning::RevertNormalMap>("General", "RevertNormalMap", "更新前恢复原法线", "立即应用于后续操作。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, false, 0, 1),
            MakeSetting<&RuntimeTuning::ApplyOverlay>("General", "ApplyOverlay", "应用到RaceMenu覆盖层", "重新生成并更新RaceMenu覆盖层法线。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::RemoveSkinOverrides>("General", "RemoveSkinOverrides", "移除皮肤法线覆盖", "高级：生成时移除已有NiOverride法线覆盖；关闭选项不会重建已移除的外部覆盖。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, true, 0, 1),
            MakeSetting<&RuntimeTuning::RemoveNodeOverrides>("General", "RemoveNodeOverrides", "移除节点法线覆盖", "高级：生成时移除已有NiOverride法线覆盖；关闭选项不会重建已移除的外部覆盖。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, true, 0, 1),
            MakeSetting<&RuntimeTuning::updateDelayTick>("General", "UpdateDelayTick", "网格更新等待帧数", "网格变化后等待0–255帧，让装备和形变渲染完成。", SettingType::Integer, 0, 255, "", ApplyPolicy::Immediate, false, 0, 255),
            MakeSetting<&RuntimeTuning::WeldDistance>("Geometry", "WeldDistance", "顶点焊接距离", "焊接距离，过大可能连接错误顶点；菜单建议0–1，0会按最小浮点精度处理。", SettingType::Float, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 100000),
            MakeSetting<&RuntimeTuning::BoundaryWeldDistance>("Geometry", "BoundaryWeldDistance", "边界焊接距离", "焊接距离，过大可能连接错误顶点；菜单建议0–1，0会按最小浮点精度处理。", SettingType::Float, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 100000),
            MakeSetting<&RuntimeTuning::WeldAccuracy>("Geometry", "WeldAccuracy", "提高焊接精度", "更改后重新生成法线。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::NormalSmoothDegree>("Geometry", "NormalSmoothDegree", "法线平滑角度", "作者范围：通常0–180度；允许反向法线时0–90度。", SettingType::Float, 0, 180, "", ApplyPolicy::Regenerate, false, 0, 180),
            MakeSetting<&RuntimeTuning::AllowInvertNormalSmooth>("Geometry", "AllowInvertNormalSmooth", "允许反向法线参与平滑", "更改后重新生成法线。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::Subdivision>("Geometry", "Subdivision", "三角形细分次数", "菜单建议范围，非作者硬上限；较大值会显著增加计算量。每次细分使三角形数量约乘4。", SettingType::Integer, 0, 3, "", ApplyPolicy::Regenerate, false, 0, 255),
            MakeSetting<&RuntimeTuning::SubdivisionTriThreshold>("Geometry", "SubdivisionTriThreshold", "细分三角形数量阈值", "限制参与细分的三角形数量；较高值会增加内存和处理时间。", SettingType::Integer, 0, 1000000, "", ApplyPolicy::Regenerate, false, 0, 2147483647),
            MakeSetting<&RuntimeTuning::SubdivisionVertexSmooth>("Geometry", "SubdivisionVertexSmooth", "每次细分后的顶点平滑次数", "菜单建议范围，非作者硬上限；较大值会显著增加计算量。反复平滑可能抹去网格细节。", SettingType::Integer, 0, 10, "", ApplyPolicy::Regenerate, false, 0, 255),
            MakeSetting<&RuntimeTuning::SubdivisionVertexSmoothStrength>("Geometry", "SubdivisionVertexSmoothStrength", "细分顶点平滑强度", "更改后重新生成法线。", SettingType::Float, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::VertexSmooth>("Geometry", "VertexSmooth", "全部顶点平滑次数", "菜单建议范围，非作者硬上限；较大值会显著增加计算量。反复平滑可能抹去网格细节。", SettingType::Integer, 0, 10, "", ApplyPolicy::Regenerate, false, 0, 255),
            MakeSetting<&RuntimeTuning::VertexSmoothStrength>("Geometry", "VertexSmoothStrength", "全部顶点平滑强度", "更改后重新生成法线。", SettingType::Float, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::VertexSmoothByAngle>("Geometry", "VertexSmoothByAngle", "按角度平滑次数", "菜单建议范围，非作者硬上限；较大值会显著增加计算量。反复平滑可能抹去网格细节。", SettingType::Integer, 0, 10, "", ApplyPolicy::Regenerate, false, 0, 255),
            MakeSetting<&RuntimeTuning::VertexSmoothByAngleThreshold1>("Geometry", "VertexSmoothByAngleThreshold1", "角度平滑起始阈值", "作者范围0–90度；起始阈值必须小于最大强度阈值。", SettingType::Float, 0, 90, "", ApplyPolicy::Regenerate, false, 0, 90),
            MakeSetting<&RuntimeTuning::VertexSmoothByAngleThreshold2>("Geometry", "VertexSmoothByAngleThreshold2", "角度平滑最大强度阈值", "作者范围0–90度；起始阈值必须小于最大强度阈值。", SettingType::Float, 0, 90, "", ApplyPolicy::Regenerate, false, 0, 90),
            MakeSetting<&RuntimeTuning::TextureWidth>("Texture", "TextureWidth", "最小纹理宽度", "D3D11纹理尺寸1–16384；建议使用2的幂，较高分辨率增加显存和生成耗时。", SettingType::Integer, 1, 16384, "", ApplyPolicy::Regenerate, false, 1, 16384),
            MakeSetting<&RuntimeTuning::TextureHeight>("Texture", "TextureHeight", "最小纹理高度", "D3D11纹理尺寸1–16384；建议使用2的幂，较高分辨率增加显存和生成耗时。", SettingType::Integer, 1, 16384, "", ApplyPolicy::Regenerate, false, 1, 16384),
            MakeSetting<&RuntimeTuning::TangentZCorrection>("Texture", "TangentZCorrection", "修正切线法线蓝色通道", "更改后重新生成法线。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::DetailStrength>("Texture", "DetailStrength", "细节法线强度", "更改后重新生成法线。", SettingType::Float, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::IgnoreMissingNormalMap>("Texture", "IgnoreMissingNormalMap", "原法线缺失时仍生成", "更改后重新生成法线。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::GPUEnable>("Performance", "GPUEnable", "启用GPU生成", "保存后重启游戏生效，避免运行期间重建设备或后台检测资源。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Restart, false, 0, 1),
            MakeSetting<&RuntimeTuning::GPUDeviceIndex>("Performance", "GPUDeviceIndex", "GPU设备索引", "保存后重启游戏生效，避免运行期间重建设备或后台检测资源。 -1自动选择第二GPU，0为主GPU，正数为设备索引。", SettingType::Integer, -1, 16, "", ApplyPolicy::Restart, false, -1, 2147483647),
            MakeSetting<&RuntimeTuning::GPUForceSync>("Performance", "GPUForceSync", "强制GPU同步", "立即应用于后续操作。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, false, 0, 1),
            MakeSetting<&RuntimeTuning::UpdateDistanceVramSave>("Performance", "UpdateDistanceVramSave", "释放远处角色显存", "立即应用于后续操作。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, false, 0, 1),
            MakeSetting<&RuntimeTuning::UseMipMap>("Performance", "UseMipMap", "生成Mip贴图", "附加Mip层约增加三分之一显存，可能改善采样性能；更改后重新生成。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::AutoTaskQ>("Performance", "AutoTaskQ", "任务队列预设", "等待所有更新完成后，重新配置任务队列和CPU处理资源。 立即模式可能阻塞前台；0–3预设不使用任务间隔，4/5预设使用任务间隔。", SettingType::Choice, 0, 5, "立即|最快|较快|均衡|较省性能|最省性能", ApplyPolicy::IdleReconfigure, false, 0, 5),
            MakeSetting<&RuntimeTuning::EcoreUse>("Performance", "EcoreUse", "优先使用能效核心", "等待所有更新完成后，重新配置任务队列和CPU处理资源。", SettingType::Boolean, 0, 1, "", ApplyPolicy::IdleReconfigure, false, 0, 1),
            MakeSetting<&RuntimeTuning::ProcessingInLoading>("Performance", "ProcessingInLoading", "加载画面中预处理", "保存后重启游戏生效，避免运行期间重建设备或后台检测资源。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Restart, false, 0, 1),
            MakeSetting<&RuntimeTuning::ProcessingInLoadingWithMainGPU>("Performance", "ProcessingInLoadingWithMainGPU", "加载画面使用主GPU", "保存后重启游戏生效，避免运行期间重建设备或后台检测资源。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Restart, false, 0, 1),
            MakeSetting<&RuntimeTuning::TaskQTickMS>("Performance", "TaskQTickMS", "GPU任务间隔毫秒", "等待所有更新完成后，重新配置任务队列和CPU处理资源。 0表示每帧处理队列；只对4/5预设生效，0–3预设忽略本项。", SettingType::Integer, 0, 60000, "", ApplyPolicy::IdleReconfigure, false, 0, 60000),
            MakeSetting<&RuntimeTuning::TextureCompress>("Performance", "TextureCompress", "纹理压缩方式", "-1自动，0不压缩，1 CPU BC7，2 GPU BC7；主GPU压缩不支持后台处理；GPU方式仍有纹理回读成本，不保证更快。更改后重新生成。", SettingType::Choice, -1, 2, "自动|不压缩|CPU BC7|GPU BC7", ApplyPolicy::Regenerate, false, -1, 2),
            MakeSetting<&RuntimeTuning::TextureCompressQuality>("Performance", "TextureCompressQuality", "纹理压缩质量", "作者范围0–7：较高值提高质量并延长压缩时间；更改后重新生成。", SettingType::Integer, 0, 7, "", ApplyPolicy::Regenerate, false, 0, 7),
            MakeSetting<&RuntimeTuning::SIMDtype>("Performance", "SIMDtype", "CPU压缩指令集", "等待所有更新完成后，重新配置任务队列和CPU处理资源。 实际指令集仍按CPU支持情况回退。", SettingType::Choice, 0, 4, "自动|SSE2|SSE4|AVX|AVX2", ApplyPolicy::IdleReconfigure, false, 0, 4),
            MakeSetting<&RuntimeTuning::DiskCache>("Performance", "DiskCache", "启用磁盘缓存", "立即应用于后续操作。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, false, 0, 1),
            MakeSetting<&RuntimeTuning::DiskCacheFolder>("Performance", "DiskCacheFolder", "磁盘缓存目录", "保存后重启游戏生效，避免运行期间重建设备或后台检测资源。", SettingType::String, 0, 0, "", ApplyPolicy::Restart, false, 0, 0),
            MakeSetting<&RuntimeTuning::DiskCacheLimitMB>("Performance", "DiskCacheLimitMB", "磁盘缓存上限MB", "磁盘缓存容量MB；更大可保存更多生成结果。", SettingType::Integer, 0, 65536, "", ApplyPolicy::Immediate, false, 0, 2147483647),
            MakeSetting<&RuntimeTuning::ClearDiskCache>("Performance", "ClearDiskCache", "退出时清理磁盘缓存", "立即应用于后续操作。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, false, 0, 1),
            MakeSetting<&RuntimeTuning::DiskCacheHashPrecision>("Performance", "DiskCacheHashPrecision", "缓存哈希精度指数", "高级：保存的是2的幂指数（0–30），原默认9即512；更改会影响几何哈希，通常无需修改。", SettingType::Integer, 0, 30, "", ApplyPolicy::Regenerate, true, 0, 30),
            MakeSetting<&RuntimeTuning::updateDistance>("Performance", "UpdateDistance", "法线更新距离", "游戏单位，约100单位为2米；0表示无限距离。更新距离会至少等于检测距离；任一无限时更新也无限。", SettingType::Float, 0, 100000, "", ApplyPolicy::Immediate, false, 0, 100000),
            MakeSetting<&RuntimeTuning::RealtimeDetect>("RealtimeDetect", "RealtimeDetect", "启用实时网格检测", "重新登记角色并重新生成，后续变化按此选项检测。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::RealtimeDetectHead>("RealtimeDetect", "RealtimeDetectHead", "头部实时检测模式", "模式2检测更多头部变化，可能增加更新频率和开销。", SettingType::Choice, 0, 2, "关闭|仅捏脸数据|含面部表情", ApplyPolicy::Regenerate, false, 0, 2),
            MakeSetting<&RuntimeTuning::RealtimeDetectOnBackGround>("RealtimeDetect", "RealtimeDetectOnBackGround", "在后台线程检测网格", "保存后重启游戏生效，避免运行期间重建设备或后台检测资源。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Restart, false, 0, 1),
            MakeSetting<&RuntimeTuning::DetectTextureChange>("RealtimeDetect", "DetectTextureChange", "检测纹理变化", "重新登记角色并重新生成，后续变化按此选项检测。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, false, 0, 1),
            MakeSetting<&RuntimeTuning::detectDistance>("RealtimeDetect", "DetectDistance", "实时检测距离", "游戏单位，约100单位为2米；0表示无限距离。更新距离会至少等于检测距离；任一无限时更新也无限。", SettingType::Float, 0, 100000, "", ApplyPolicy::Immediate, false, 0, 100000),
            MakeSetting<&RuntimeTuning::detectTickMS>("RealtimeDetect", "DetectTickMS", "实时检测间隔毫秒", "实时检测间隔0–60000毫秒；0表示不额外等待。", SettingType::Integer, 0, 60000, "", ApplyPolicy::Immediate, false, 0, 60000),
            MakeSetting<&RuntimeTuning::logLevel>("Debug", "logLevel", "日志级别", "日志级别立即生效；trace/debug可能产生大量日志。", SettingType::Choice, 0, 6, "trace|debug|info|warning|error|critical|off", ApplyPolicy::Immediate, true, 0, 6),
            MakeSetting<&RuntimeTuning::flushLevel>("Debug", "flushLevel", "日志刷盘级别", "达到此级别时立即刷新日志文件。", SettingType::Choice, 0, 6, "trace|debug|info|warning|error|critical|off", ApplyPolicy::Immediate, true, 0, 6),
            MakeSetting<&RuntimeTuning::DebugTexture>("Debug", "DebugTexture", "显示法线调试纹理", "仅用于诊断：将法线显示为漫反射纹理，更改后重新生成。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Regenerate, true, 0, 1),
            MakeSetting<&RuntimeTuning::TickTimeToRealFPS>("Debug", "TickTimeToRealFPS", "按当前帧率显示计时", "诊断开关，配合相应日志级别使用；频繁记录会增加开销。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, true, 0, 1),
            MakeSetting<&RuntimeTuning::QueueTime>("Debug", "QueueTime", "队列耗时", "诊断开关，配合相应日志级别使用；频繁记录会增加开销。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, true, 0, 1),
            MakeSetting<&RuntimeTuning::FullUpdateTime>("Debug", "FullUpdateTime", "完整更新耗时", "诊断开关，配合相应日志级别使用；频繁记录会增加开销。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, true, 0, 1),
            MakeSetting<&RuntimeTuning::ActorVertexHasherTime1>("Debug", "ActorVertexHasherTime1", "角色网格哈希耗时一", "诊断开关，配合相应日志级别使用；频繁记录会增加开销。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, true, 0, 1),
            MakeSetting<&RuntimeTuning::ActorVertexHasherTime2>("Debug", "ActorVertexHasherTime2", "角色网格哈希耗时二", "诊断开关，配合相应日志级别使用；频繁记录会增加开销。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, true, 0, 1),
            MakeSetting<&RuntimeTuning::GeometryDataTime>("Debug", "GeometryDataTime", "几何数据处理耗时", "诊断开关，配合相应日志级别使用；频繁记录会增加开销。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, true, 0, 1),
            MakeSetting<&RuntimeTuning::UpdateNormalMapTime1>("Debug", "UpdateNormalMapTime1", "法线更新耗时一", "诊断开关，配合相应日志级别使用；频繁记录会增加开销。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, true, 0, 1),
            MakeSetting<&RuntimeTuning::UpdateNormalMapTime2>("Debug", "UpdateNormalMapTime2", "法线更新耗时二", "诊断开关，配合相应日志级别使用；频繁记录会增加开销。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, true, 0, 1),
            MakeSetting<&RuntimeTuning::MergeTime>("Debug", "MergeTime", "纹理合并耗时", "诊断开关，配合相应日志级别使用；频繁记录会增加开销。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, true, 0, 1),
            MakeSetting<&RuntimeTuning::GenerateMipsTime>("Debug", "GenerateMipsTime", "Mip生成耗时", "诊断开关，配合相应日志级别使用；频繁记录会增加开销。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, true, 0, 1),
            MakeSetting<&RuntimeTuning::BleedTextureTime1>("Debug", "BleedTextureTime1", "边缘扩展耗时一", "诊断开关，配合相应日志级别使用；频繁记录会增加开销。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, true, 0, 1),
            MakeSetting<&RuntimeTuning::BleedTextureTime2>("Debug", "BleedTextureTime2", "边缘扩展耗时二", "诊断开关，配合相应日志级别使用；频繁记录会增加开销。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, true, 0, 1),
            MakeSetting<&RuntimeTuning::CompressTime>("Debug", "CompressTime", "纹理压缩耗时", "诊断开关，配合相应日志级别使用；频繁记录会增加开销。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, true, 0, 1),
            MakeSetting<&RuntimeTuning::TextureCopyTime>("Debug", "TextureCopyTime", "纹理复制耗时", "诊断开关，配合相应日志级别使用；频繁记录会增加开销。", SettingType::Boolean, 0, 1, "", ApplyPolicy::Immediate, true, 0, 1),
        };
        return catalog;
    }

    inline const SettingDescriptor* FindSetting(std::string_view section, std::string_view key) {
        for (const auto& setting : GetSettingsCatalog())
            if (section == setting.section && key == setting.key) return &setting;
        return nullptr;
    }

    // Keep uncommon startup/diagnostic knobs in the parser while presenting only useful controls.
    // INI persistence uses this same list so hidden keys retain their original text and values.
    inline const std::vector<SettingDescriptor>& GetMenuSettingsCatalog() {
        static const std::vector<SettingDescriptor> catalog = [] {
            constexpr std::string_view keys[] = {
                "PlayerEnable", "NPCEnable", "MaleEnable", "FemaleEnable", "HeadEnable", "HotKey1", "HotKey2",
                "UseConsoleRef", "RevertNormalMap", "UpdateDelayTick", "ApplyOverlay",
                "RealtimeDetect", "DetectTickMS", "DetectDistance", "RealtimeDetectHead", "DetectTextureChange",
                "GPUForceSync", "UpdateDistance", "UpdateDistanceVramSave", "UseMipMap", "AutoTaskQ", "EcoreUse",
                "TaskQTickMS", "TextureCompress", "TextureCompressQuality", "DiskCache", "DiskCacheLimitMB", "ClearDiskCache",
                "TextureWidth", "TextureHeight", "DetailStrength", "TangentZCorrection", "IgnoreMissingNormalMap",
                "WeldDistance", "BoundaryWeldDistance", "WeldAccuracy", "NormalSmoothDegree", "AllowInvertNormalSmooth",
                "VertexSmoothByAngle", "VertexSmoothByAngleThreshold1", "VertexSmoothByAngleThreshold2", "logLevel"
            };
            std::vector<SettingDescriptor> result;
            for (const auto& setting : GetSettingsCatalog()) {
                if (std::find(std::begin(keys), std::end(keys), setting.key) != std::end(keys))
                    result.push_back(setting);
            }
            return result;
        }();
        return catalog;
    }

    inline bool IsLogLevelSetting(const SettingDescriptor& setting) {
        return std::string_view(setting.section) == "Debug" &&
            (std::string_view(setting.key) == "logLevel" || std::string_view(setting.key) == "flushLevel");
    }

    inline std::string SettingValueText(const SettingDescriptor& setting, const RuntimeTuning& values) {
        const auto value = setting.read(values);
        if (const auto* b = std::get_if<bool>(&value)) return *b ? "true" : "false";
        if (const auto* i = std::get_if<int>(&value)) {
            if (IsLogLevelSetting(setting)) {
                constexpr const char* levels[] = {"trace", "debug", "info", "warning", "error", "critical", "off"};
                if (*i >= 0 && *i <= 6) return levels[*i];
            }
            return std::to_string(*i);
        }
        if (const auto* f = std::get_if<float>(&value)) {
            char buffer[64];
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), *f, std::chars_format::general,
                std::numeric_limits<float>::max_digits10);
            return result.ec == std::errc{} ? std::string(buffer, result.ptr) : std::string{};
        }
        return std::get<std::string>(value);
    }

    inline bool ValidateSettingValue(const SettingDescriptor& setting, const SettingValue& value, std::string& error) {
        double numeric = 0;
        if (const auto* number = std::get_if<int>(&value)) numeric = *number;
        else if (const auto* number = std::get_if<float>(&value)) numeric = *number;
        else if (const auto* text = std::get_if<std::string>(&value)) {
            if (text->empty() || text->find_first_of("\r\n#;") != std::string::npos || text->find('\0') != std::string::npos ||
                std::isspace(static_cast<unsigned char>(text->front())) || std::isspace(static_cast<unsigned char>(text->back()))) {
                error = std::string(setting.key) + ": 路径不能为空，或包含首尾空白、换行、NUL、#、;。";
                return false;
            }
            return true;
        } else return true;
        if (!std::isfinite(numeric) || numeric < setting.hardMinimum || numeric > setting.hardMaximum) {
            error = std::string(setting.key) + ": 数值超出有效范围。";
            return false;
        }
        return true;
    }

    inline bool ParseSettingValue(const SettingDescriptor& setting, std::string_view text,
        RuntimeTuning& values, std::string& error) {
        const auto fail = [&]() { error = std::string(setting.section) + "/" + setting.key + ": 无效值 " + std::string(text); return false; };
        SettingValue parsed;
        if (setting.type == SettingType::String) parsed = std::string(text);
        else if (setting.type == SettingType::Boolean || IsLogLevelSetting(setting)) {
            std::string lower(text);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (setting.type == SettingType::Boolean) {
                if (lower == "true" || lower == "1") parsed = true;
                else if (lower == "false" || lower == "0") parsed = false;
                else return fail();
            } else {
                constexpr std::string_view levels[] = {"trace", "debug", "info", "warning", "error", "critical", "off"};
                if (lower == "warn") lower = "warning";
                if (lower == "err") lower = "error";
                int index = 0;
                for (; index < 7 && lower != levels[index]; ++index) {}
                if (index == 7) return fail();
                parsed = index;
            }
        } else {
            auto number = text;
            if (number.starts_with('+')) number.remove_prefix(1);
            if (number.empty()) return fail();
            if (setting.type == SettingType::Float) {
                float value;
                const auto result = std::from_chars(number.data(), number.data() + number.size(), value);
                if (result.ec != std::errc{} || result.ptr != number.data() + number.size()) return fail();
                parsed = value;
            } else {
                int value;
                const auto result = std::from_chars(number.data(), number.data() + number.size(), value);
                if (result.ec != std::errc{} || result.ptr != number.data() + number.size()) return fail();
                parsed = value;
            }
        }
        if (!ValidateSettingValue(setting, parsed, error)) return false;
        if (!setting.assign(values, parsed)) return fail();
        error.clear();
        return true;
    }

    inline bool NormalizeRuntimeTuning(const RuntimeTuning& input, RuntimeTuning& normalized, std::string& error) {
        for (const auto& setting : GetSettingsCatalog())
            if (!ValidateSettingValue(setting, setting.read(input), error)) return false;
        if (input.AllowInvertNormalSmooth && input.NormalSmoothDegree > 90.0f) {
            error = "允许反向法线时，法线平滑角度必须在0–90度。"; return false;
        }
        if (input.VertexSmoothByAngleThreshold1 >= input.VertexSmoothByAngleThreshold2) {
            error = "角度平滑起始阈值必须小于最大强度阈值。"; return false;
        }
        if (input.HotKey1 != 0 && input.HotKey2 != 0 && input.HotKey1 == input.HotKey2) {
            error = "快捷键主键和修饰键不能相同。"; return false;
        }
        normalized = input;
        // Geometry processing already uses this same float precision lower bound.
        normalized.WeldDistance = std::max(normalized.WeldDistance, 1e-6f);
        normalized.BoundaryWeldDistance = std::max(normalized.BoundaryWeldDistance, 1e-6f);
        if (normalized.detectDistance <= 0.001f) normalized.detectDistance = 0;
        if (normalized.updateDistance <= 0.001f) normalized.updateDistance = 0;
        if (normalized.detectDistance == 0 || normalized.updateDistance == 0) normalized.updateDistance = 0;
        else normalized.updateDistance = std::max(normalized.updateDistance, normalized.detectDistance);
        error.clear();
        return true;
    }

    inline bool ParseRuntimeTuningIni(std::istream& stream, const RuntimeTuning& current,
        RuntimeTuning& result, std::string& error, bool loggingOnly = false) {
        RuntimeTuning candidate = current;
        std::string section;
        std::string line;
        const auto trimText = [](std::string_view text) {
            constexpr std::string_view spaces = " \t\r\n";
            const auto begin = text.find_first_not_of(spaces);
            if (begin == std::string_view::npos) return std::string_view{};
            return text.substr(begin, text.find_last_not_of(spaces) - begin + 1);
        };
        while (std::getline(stream, line)) {
            if (line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
            const auto comment = line.find_first_of("#;");
            if (comment != std::string::npos) line.erase(comment);
            const auto text = trimText(line);
            if (text.empty()) continue;
            if (text.front() == '[') {
                section = text.back() == ']' ? std::string(trimText(text.substr(1, text.size() - 2))) : std::string{};
                continue;
            }
            const auto equals = text.find('=');
            if (equals == std::string_view::npos) continue;
            const auto* setting = FindSetting(section, trimText(text.substr(0, equals)));
            if (!setting || (loggingOnly && !IsLogLevelSetting(*setting))) continue;
            // Keep the entire right hand side: "500=garbage" must fail numeric parsing.
            if (!ParseSettingValue(*setting, trimText(text.substr(equals + 1)), candidate, error)) return false;
        }
        if (stream.bad()) { error = "读取INI失败。"; return false; }
        return NormalizeRuntimeTuning(candidate, result, error);
    }

    struct RuntimeTuningChanges {
        bool immediate = false;
        bool idleReconfigure = false;
        bool regenerate = false;
        bool restart = false;
    };

    inline RuntimeTuningChanges GetRuntimeTuningChanges(const RuntimeTuning& before, const RuntimeTuning& after) {
        RuntimeTuningChanges changes;
        for (const auto& setting : GetSettingsCatalog()) {
            if (setting.read(before) == setting.read(after)) continue;
            switch (setting.policy) {
            case ApplyPolicy::Immediate: changes.immediate = true; break;
            case ApplyPolicy::IdleReconfigure: changes.idleReconfigure = true; break;
            case ApplyPolicy::Regenerate: changes.regenerate = true; break;
            case ApplyPolicy::Restart: changes.restart = true; break;
            }
        }
        return changes;
    }

    inline ApplyPolicy DiffApplyPolicy(const RuntimeTuning& before, const RuntimeTuning& after) {
        const auto changes = GetRuntimeTuningChanges(before, after);
        if (changes.restart) return ApplyPolicy::Restart;
        if (changes.regenerate) return ApplyPolicy::Regenerate;
        if (changes.idleReconfigure) return ApplyPolicy::IdleReconfigure;
        return ApplyPolicy::Immediate;
    }

    inline void CopyRestartFields(const RuntimeTuning& from, RuntimeTuning& to) {
        for (const auto& setting : GetSettingsCatalog())
            if (setting.policy == ApplyPolicy::Restart) setting.assign(to, setting.read(from));
    }

    inline std::string RuntimeGenerationSignature(const RuntimeTuning& values) {
        std::string signature = "MuGenerationV1;";
        for (const auto& setting : GetSettingsCatalog()) {
            if (setting.policy != ApplyPolicy::Regenerate) continue;
            signature += setting.section; signature += '/'; signature += setting.key;
            signature += '='; signature += SettingValueText(setting, values); signature += ';';
        }
        return signature;
    }
}
