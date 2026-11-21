#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <DirectXMath.h>
namespace Mus {
    inline DirectX::XMVECTOR NormalizeOr(DirectX::FXMVECTOR value, DirectX::FXMVECTOR fallback) {
        const float lengthSquared = DirectX::XMVectorGetX(DirectX::XMVector3LengthSq(value));
        return lengthSquared > 1e-12f && std::isfinite(lengthSquared) ?
            DirectX::XMVectorScale(value, 1.0f / std::sqrt(lengthSquared)) : fallback;
    }
    struct NormalTangents { DirectX::XMVECTOR tangent, bitangent; };
    inline NormalTangents BuildNormalTangents(DirectX::FXMVECTOR normal,
        DirectX::FXMVECTOR tangent, DirectX::FXMVECTOR bitangent) {
        using namespace DirectX;
        const auto axis = std::abs(XMVectorGetZ(normal)) < 0.9f ? XMVectorSet(0, 0, 1, 0) : XMVectorSet(0, 1, 0, 0);
        const auto fallback = NormalizeOr(XMVector3Cross(normal, axis), XMVectorSet(1, 0, 0, 0));
        const auto t = NormalizeOr(tangent, fallback);
        const auto b = NormalizeOr(bitangent, XMVector3Cross(normal, fallback));
        const auto ft = NormalizeOr(XMVectorSubtract(t, XMVectorScale(normal, XMVectorGetX(XMVector3Dot(normal, t)))), fallback);
        const float handedness = XMVectorGetX(XMVector3Dot(XMVector3Cross(normal, t), b)) < 0.0f ? -1.0f : 1.0f;
        return {ft, XMVectorScale(NormalizeOr(XMVector3Cross(normal, ft), XMVector3Cross(normal, fallback)), handedness)};
    }
    inline std::uint32_t WrapTextureCoordinate(std::int32_t coordinate, std::uint32_t size) {
        if (!size) return 0;
        if ((size & (size - 1)) == 0) return static_cast<std::uint32_t>(coordinate) & (size - 1);
        const auto remainder = coordinate % static_cast<std::int32_t>(size);
        return static_cast<std::uint32_t>(remainder < 0 ? remainder + size : remainder);
    }
    inline DirectX::XMVECTOR SlerpUnitVector(DirectX::FXMVECTOR a, DirectX::FXMVECTOR b, float t) {
        using namespace DirectX;
        const auto dot = std::clamp(XMVectorGetX(XMVector3Dot(a, b)), -1.0f, 1.0f);
        if (dot > 0.9995f) return XMVector3Normalize(XMVectorLerp(a, b, t));
        auto relative = XMVectorSubtract(b, XMVectorScale(a, dot));
        if (XMVectorGetX(XMVector3LengthSq(relative)) < 1e-12f) relative = XMVector3Orthogonal(a);
        relative = XMVector3Normalize(relative);
        const auto angle = std::acos(dot) * t;
        return XMVector3Normalize(XMVectorAdd(XMVectorScale(a, std::cos(angle)), XMVectorScale(relative, std::sin(angle))));
    }
}
