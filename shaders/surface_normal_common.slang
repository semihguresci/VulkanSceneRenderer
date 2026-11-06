#pragma once
#include "brdf_common.slang"

struct SurfaceNormalState
{
    float3 geometricNormal;
    float3 shadingNormal;
    float3 tangent;
    float3 bitangent;
};

float3 FallbackTangent(float3 normal)
{
    float3 axis = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0)
                                        : float3(1.0, 0.0, 0.0);
    float3 tangent = cross(axis, normal);
    if (dot(tangent, tangent) < 1e-8)
    {
        tangent = cross(float3(0.0, 0.0, 1.0), normal);
    }
    return SafeNormalize(tangent, float3(1.0, 0.0, 0.0));
}

float3 BuildWorldNormal(float3x3 normalMatrix, float3 objectNormal)
{
    return SafeNormalize(mul(normalMatrix, objectNormal), float3(0.0, 1.0, 0.0));
}

float4 BuildWorldTangent(float3x3 modelMatrix, float3 worldNormal,
                         float4 objectTangent)
{
    float3 tangent = mul(modelMatrix, objectTangent.xyz);
    tangent -= worldNormal * dot(worldNormal, tangent);
    tangent = SafeNormalize(tangent, FallbackTangent(worldNormal));

    float tangentHandedness = abs(objectTangent.w) > 0.5 ? objectTangent.w : 1.0;
    float modelHandedness = determinant(modelMatrix) < 0.0 ? -1.0 : 1.0;
    tangentHandedness = tangentHandedness * modelHandedness;
    tangentHandedness = tangentHandedness < 0.0 ? -1.0 : 1.0;
    return float4(tangent, tangentHandedness);
}

SurfaceNormalState EvaluateSurfaceNormal(float3 worldNormal, float4 worldTangent,
                                         float2 texCoord, uint normalTextureIndex,
                                         float normalTextureScale, float faceSign)
{
    SurfaceNormalState state;
    float orientedFaceSign = faceSign < 0.0 ? -1.0 : 1.0;
    state.geometricNormal =
        SafeNormalize(worldNormal, float3(0.0, 1.0, 0.0)) * orientedFaceSign;
    state.shadingNormal = state.geometricNormal;
    state.tangent = 0.0.xxx;
    state.bitangent = 0.0.xxx;

    if (normalTextureIndex == 0xffffffff)
    {
        return state;
    }

    float3 tangent = worldTangent.xyz;
    tangent -= state.geometricNormal * dot(state.geometricNormal, tangent);
    tangent = SafeNormalize(tangent, FallbackTangent(state.geometricNormal));

    float tangentSign = abs(worldTangent.w) > 0.5 ? worldTangent.w : 1.0;
    float3 bitangent = cross(state.geometricNormal, tangent) * tangentSign;
    bitangent = SafeNormalize(
        bitangent,
        cross(state.geometricNormal, FallbackTangent(state.geometricNormal)) *
            tangentSign);

    state.tangent = tangent;
    state.bitangent = bitangent;

    float3 normalSample =
        SamplePbrTexture(normalTextureIndex, texCoord, 1.0.xxxx).xyz * 2.0 - 1.0;
    normalSample.xy *= normalTextureScale;
    normalSample = SafeNormalize(normalSample, float3(0.0, 0.0, 1.0));
    state.shadingNormal = SafeNormalize(
        tangent * normalSample.x + bitangent * normalSample.y +
            state.geometricNormal * normalSample.z,
        state.geometricNormal);
    if (dot(state.shadingNormal, state.geometricNormal) < 0.0)
    {
        state.shadingNormal *= -1.0;
    }

    return state;
}

SurfaceNormalState EvaluateSurfaceNormal(float3 worldNormal, float4 worldTangent,
                                         float2 texCoord, uint normalTextureIndex,
                                         float normalTextureScale)
{
    return EvaluateSurfaceNormal(worldNormal, worldTangent, texCoord,
                                 normalTextureIndex, normalTextureScale, 1.0);
}

float3 BuildAnisotropyTangent(SurfaceNormalState surfaceNormal,
                              float4 worldTangent, float2 textureRG,
                              float rotation)
{
    float3 geometricNormal = SafeNormalize(
        surfaceNormal.geometricNormal, float3(0.0, 1.0, 0.0));
    float3 tangent = worldTangent.xyz;
    tangent -= geometricNormal * dot(geometricNormal, tangent);
    tangent = SafeNormalize(tangent, FallbackTangent(geometricNormal));
    // BuildWorldTangent already folds sign(det(model)) into TANGENT.w.
    float tangentSign = FiniteScalarOr(worldTangent.w, 1.0) < 0.0 ? -1.0 : 1.0;
    float3 bitangent = cross(geometricNormal, tangent) * tangentSign;

    float2 direction = Finite2Or(textureRG, float2(1.0, 0.5)) * 2.0 - 1.0;
    direction = SafeNormalize(float3(direction, 0.0), float3(1.0, 0.0, 0.0)).xy;
    float angle = FiniteScalarOr(rotation, 0.0);
    float c = cos(angle), s = sin(angle);
    // Positive rotation is counterclockwise from authored T toward authored B;
    // a mirrored transform changes B through the tangent sign, not the angle.
    float2 rotated = float2(c * direction.x - s * direction.y,
                            s * direction.x + c * direction.y);
    float3 result = tangent * rotated.x + bitangent * rotated.y;
    float3 shadingNormal = SafeNormalize(surfaceNormal.shadingNormal, geometricNormal);
    result -= shadingNormal * dot(shadingNormal, result);
    return SafeNormalize(result, FallbackTangent(shadingNormal));
}
