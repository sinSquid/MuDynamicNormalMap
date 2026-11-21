#include "../include/NormalMath.h"
#include <cassert>
#include <limits>

int main() {
    using namespace DirectX;
    const auto x = XMVectorSet(1, 0, 0, 0), y = XMVectorSet(0, 1, 0, 0), z = XMVectorSet(0, 0, 1, 0);
    const auto n = Mus::NormalizeOr(XMVectorZero(), z);
    assert(XMVector3Equal(n, z));
    const auto invalid = XMVectorSet(std::numeric_limits<float>::quiet_NaN(), 0, 0, 0);
    assert(XMVector3Equal(Mus::NormalizeOr(invalid, z), z));
    for (const float mirror : {1.0f, -1.0f}) {
        const auto frame = Mus::BuildNormalTangents(z, XMVectorScale(x, mirror), y);
        assert(XMVector3NearEqual(frame.tangent, XMVectorScale(x, mirror), XMVectorReplicate(1e-6f)));
        assert(XMVector3NearEqual(frame.bitangent, y, XMVectorReplicate(1e-6f)));
        const auto tbn = XMMATRIX(frame.tangent, frame.bitangent, n, XMVectorSet(0, 0, 0, 1));
        assert(XMVector3NearEqual(XMVector3TransformNormal(y, tbn), y, XMVectorReplicate(1e-6f)));
    }
    for (const auto tangent : {XMVectorZero(), z, invalid}) {
        const auto frame = Mus::BuildNormalTangents(n, tangent, XMVectorZero());
        assert(std::isfinite(XMVectorGetX(frame.tangent)) && std::isfinite(XMVectorGetY(frame.bitangent)));
        assert(std::abs(XMVectorGetX(XMVector3Length(frame.tangent)) - 1.0f) < 1e-6f);
        assert(std::abs(XMVectorGetX(XMVector3Length(frame.bitangent)) - 1.0f) < 1e-6f);
        assert(std::abs(XMVectorGetX(XMVector3Dot(n, frame.tangent))) < 1e-6f);
    }
    assert(XMVector3Equal(Mus::NormalizeOr(XMVectorLerp(z, XMVectorNegate(z), 0.5f), z), z));
}
