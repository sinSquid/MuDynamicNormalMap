cbuffer ConstBuffer : register(b0)
{
    uint texWidth;
    uint texHeight;
    uint indicesStart;
    uint indicesEnd;

    uint hasSrcTexture;
    uint hasDetailTexture;
    uint hasOverlayTexture;
    uint hasMaskTexture;

    uint tangentZCorrection;
    float detailStrength;
    uint vertexEnd;
    uint ownershipPass; // 0: claim pixels; 1: shade only the final owner.
};

StructuredBuffer<float3> vertices   : register(t0); // a_data->vertices
StructuredBuffer<float2> uvs        : register(t1); // a_data->uvs
StructuredBuffer<float3> normals    : register(t2); // a_data->normals
StructuredBuffer<float3> tangent    : register(t3); // a_data->tangent
StructuredBuffer<float3> bitangent  : register(t4); // a_data->bitangent
StructuredBuffer<uint>   indices    : register(t5); // a_data->indices

Texture2D<float4> srcTexture        : register(t6);
Texture2D<float4> detailTexture     : register(t7);
Texture2D<float4> overlayTexture    : register(t8);
Texture2D<float4> maskTexture       : register(t9);

RWTexture2D<float4> dstTexture      : register(u0);
RWTexture2D<uint> pixelOwner        : register(u1);

SamplerState samplerState           : register(s0);

float EdgeFunction(float2 a, float2 b, float2 c)
{
    return (c.x - a.x) * (b.y - a.y) - (c.y - a.y) * (b.x - a.x);
}

bool IsTopLeft(float2 a, float2 b)
{
    float2 e = b - a;
    return (e.y > 0) || (e.y == 0 && e.x < 0);
}

uint WrapCoordinate(int coordinate, uint size)
{
    if ((size & (size - 1)) == 0) return (uint)coordinate & (size - 1);
    int remainder = coordinate % (int)size;
    return (uint)(remainder < 0 ? remainder + (int)size : remainder);
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    float lengthSquared = dot(value, value);
    return lengthSquared > 1e-12f && isfinite(lengthSquared) ? value * rsqrt(lengthSquared) : fallback;
}

[numthreads(64, 1, 1)]
void CSMain(uint3 threadID : SV_DispatchThreadID)
{
    if (texWidth == 0 || texHeight == 0) return;
    const uint index = indicesStart + threadID.x * 3;
    if (index + 2 >= indicesEnd)
        return;

    uint i0 = indices[index + 0];
    uint i1 = indices[index + 1];
    uint i2 = indices[index + 2];

    if (i0 >= vertexEnd || i1 >= vertexEnd || i2 >= vertexEnd)
        return;
    //float3 v0 = vertices[i0];
    //float3 v1 = vertices[i1];
    //float3 v2 = vertices[i2];

    float2 uv0 = uvs[i0];
    float2 uv1 = uvs[i1];
    float2 uv2 = uvs[i2];

    float3 n0 = normals[i0];
    float3 n1 = normals[i1];
    float3 n2 = normals[i2];

    float3 t0 = tangent[i0];
    float3 t1 = tangent[i1];
    float3 t2 = tangent[i2];

    float3 b0 = bitangent[i0];
    float3 b1 = bitangent[i1];
    float3 b2 = bitangent[i2];

    float2 p0 = uv0 * float2(texWidth, texHeight);
    float2 p1 = uv1 * float2(texWidth, texHeight);
    float2 p2 = uv2 * float2(texWidth, texHeight);

    float area = EdgeFunction(p0, p1, p2);
    if (abs(area) < 1e-6)
        return;
	float invArea = rcp(area);
	bool isCCW = area > 0.0f;

    float minX = floor(min(min(p0.x, p1.x), p2.x));
    float minY = floor(min(min(p0.y, p1.y), p2.y));
    float maxX = ceil(max(max(p0.x, p1.x), p2.x));
    float maxY = ceil(max(max(p0.y, p1.y), p2.y));

    float2 minBB = min(p0, min(p1, p2));
    float2 maxBB = max(p0, max(p1, p2));
    int2 minP = (int2)floor(minBB);
    int2 maxP = (int2)ceil(maxBB);
    if (any((maxP - minP) >= int2(texWidth * 2, texHeight * 2)))
        return;

    float2 invTexSize = rcp(float2(texWidth, texHeight));

    for (int y = minP.y; y <= maxP.y; y++)
    {
        for (int x = minP.x; x <= maxP.x; x++)
        {
            float2 xy = float2((float)x, (float)y);
            float2 p = xy + float2(0.5f, 0.5f);

            float w0 = EdgeFunction(p1, p2, p);
            float w1 = EdgeFunction(p2, p0, p);
            float w2 = EdgeFunction(p0, p1, p);

            if (isCCW)
            {
                if (!(w0 > 0 || (w0 == 0 && IsTopLeft(p1, p2)))) 
                    continue;
                if (!(w1 > 0 || (w1 == 0 && IsTopLeft(p2, p0)))) 
                    continue;
                if (!(w2 > 0 || (w2 == 0 && IsTopLeft(p0, p1)))) 
                    continue;
            }
            else
            {
                float cw0 = -w0;
                float cw1 = -w1;
                float cw2 = -w2;
                if (!(cw0 > 0 || (cw0 == 0 && IsTopLeft(p2, p1)))) 
                    continue;
                if (!(cw1 > 0 || (cw1 == 0 && IsTopLeft(p0, p2)))) 
                    continue;
                if (!(cw2 > 0 || (cw2 == 0 && IsTopLeft(p1, p0))))
                    continue;
			}

            uint2 wrapXY = uint2(WrapCoordinate(x, texWidth), WrapCoordinate(y, texHeight));
            // The absolute triangle index is independent of dispatch chunking.
            // Zero is the clear sentinel; the highest triangle index wins.
            uint trianglePriority = index / 3 + 1;
            if (ownershipPass == 0)
            {
                InterlockedMax(pixelOwner[wrapXY], trianglePriority);
                continue;
            }
            if (pixelOwner[wrapXY] != trianglePriority)
                continue;

            float3 bary = float3(w0, w1, w2) * invArea;
            float2 uv = xy * invTexSize;
            float4 dstColor = float4(0.0f, 0.0f, 0.0f, 1.0f);
            float4 overlayColor = float4(1.0f, 1.0f, 1.0f, 0.0f);
            if (hasOverlayTexture > 0)
            {
                overlayColor = overlayTexture.SampleLevel(samplerState, uv, 0);
            }
            if (overlayColor.a < 1.0f)
            {
                float3 n = SafeNormalize(n0 * bary.x + n1 * bary.y + n2 * bary.z, float3(0, 0, 1));
                float4 maskColor = float4(0.5f, 0.5f, 0.5f, 0.0f);
                if (hasMaskTexture > 0 && hasSrcTexture > 0)
                {
                    maskColor = maskTexture.SampleLevel(samplerState, uv, 0);
                }
                if (maskColor.a < 1.0f)
                {
                    float4 detailColor = float4(0.5f, 0.5f, 1.0f, 0.5f);
                    if (hasDetailTexture > 0)
                    {
                        detailColor = detailTexture.SampleLevel(samplerState, uv, 0);
                        detailColor = lerp(float4(0.5f, 0.5f, 1.0f, detailColor.a), detailColor, detailStrength);
                    }

                    float3 normalResult;
                    if (detailColor.a > 0.0f)
                    {
                        float3 tangentFallback = SafeNormalize(cross(n, abs(n.z) < 0.9f ? float3(0, 0, 1) : float3(0, 1, 0)), float3(1, 0, 0));
                        float3 t = SafeNormalize(t0 * bary.x + t1 * bary.y + t2 * bary.z, tangentFallback);
                        float3 b = SafeNormalize(b0 * bary.x + b1 * bary.y + b2 * bary.z, cross(n, tangentFallback));

                        float3 ft = SafeNormalize(t - n * dot(n, t), tangentFallback);
                        float handedness = dot(cross(n, t), b) < 0.0f ? -1.0f : 1.0f;
                        float3 fb = SafeNormalize(cross(n, ft), cross(n, tangentFallback)) * handedness;
                        float3x3 tbn = float3x3(ft, fb, n);

                        float3 srcN = float3(detailColor.rgb * 2.0f - 1.0f);
                        if (tangentZCorrection)
                        {
                            srcN.z = sqrt(max(0.0f, 1.0f - srcN.x * srcN.x - srcN.y * srcN.y));
                        }

                        float3 detailNormal = SafeNormalize(mul(srcN, tbn), n);
                        normalResult = SafeNormalize(lerp(n, detailNormal, detailColor.a), n);
                    }
                    else
                    {
                        normalResult = n;
                    }

                    float3 finalNormal = normalResult * 0.5f + 0.5f;
                    dstColor.rgb = finalNormal.xzy;
                }
                if (maskColor.a > 0.0f && hasSrcTexture > 0)
                {
					float4 srcColor = srcTexture.SampleLevel(samplerState, uv, 0);
					dstColor.rgb = lerp(dstColor.rgb, srcColor.rgb, maskColor.a);
                }
            }
            if (overlayColor.a > 0.0f)
            {
                dstColor.rgb = lerp(dstColor.rgb, overlayColor.rgb, overlayColor.a);
            }

            dstTexture[wrapXY] = float4(dstColor.rgb, 1.0f);
        }
    }
}
