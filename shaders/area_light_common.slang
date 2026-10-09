// area_light_common.slang - Shared sampled area-light helpers.
#pragma once

#include "lighting_structs.slang"
#include "brdf_common.slang"

static const uint AREA_LIGHT_INTEGRATION_SAMPLE_COUNT = 25u;
static const float AREA_LIGHT_MIN_DISTANCE_SQ = 0.01;

bool AreaLightHasRenderableRadiance(AreaLightData light)
{
    float3 radiance = Finite3Or(
        light.colorIntensity.rgb * light.colorIntensity.a, 0.0.xxx);
    return any(radiance > 0.0.xxx);
}

struct AreaLightFrame
{
    float3 position;
    float3 normal;
    float3 tangent;
    float3 bitangent;
    float2 halfSize;
    float area;
    float type;
};

float3 AreaLightFallbackTangent(float3 normal)
{
    float3 up = abs(normal.y) < 0.999 ? float3(0.0, 1.0, 0.0)
                                      : float3(1.0, 0.0, 0.0);
    return SafeNormalize(cross(up, normal), float3(1.0, 0.0, 0.0));
}

AreaLightFrame BuildAreaLightFrame(AreaLightData light)
{
    AreaLightFrame frame;
    frame.position = light.positionRange.xyz;
    frame.normal =
        SafeNormalize(light.directionType.xyz, float3(0.0, 0.0, -1.0));

    float3 tangentHint = light.tangentHalfSize.xyz -
        frame.normal * dot(light.tangentHalfSize.xyz, frame.normal);
    frame.tangent =
        SafeNormalize(tangentHint, AreaLightFallbackTangent(frame.normal));

    float3 bitangentHint = light.bitangentHalfSize.xyz -
        frame.normal * dot(light.bitangentHalfSize.xyz, frame.normal) -
        frame.tangent * dot(light.bitangentHalfSize.xyz, frame.tangent);
    frame.bitangent =
        SafeNormalize(bitangentHint,
                      SafeNormalize(cross(frame.normal, frame.tangent),
                                    float3(0.0, 1.0, 0.0)));

    frame.type = light.directionType.w;
    if (abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5)
    {
        float radius = max(light.tangentHalfSize.w, 1.0e-3);
        frame.halfSize = radius.xx;
        frame.area = PI * radius * radius;
    }
    else
    {
        frame.halfSize = float2(max(light.tangentHalfSize.w, 1.0e-3),
                                max(light.bitangentHalfSize.w, 1.0e-3));
        frame.area = 4.0 * frame.halfSize.x * frame.halfSize.y;
    }

    return frame;
}

float2 AreaLightRectSampleOffset(uint sampleIndex)
{
    uint index = sampleIndex % AREA_LIGHT_INTEGRATION_SAMPLE_COUNT;
    return float2(AreaLightGauss5Node(index % 5u),
                  AreaLightGauss5Node(index / 5u));
}

float2 AreaLightDiskSampleOffset(uint sampleIndex)
{
    // Gauss-Legendre integration in squared radius has the correct disk
    // area measure: dA = pi * d(radius^2). Five uniform angles per ring.
    uint index = sampleIndex % AREA_LIGHT_INTEGRATION_SAMPLE_COUNT;
    float radius = sqrt(0.5 * (AreaLightGauss5Node(index / 5u) + 1.0));
    float angle = 2.0 * PI * float(index % 5u) / 5.0;
    return radius * float2(cos(angle), sin(angle));
}

float AreaLightGauss5Node(uint index)
{
    static const float nodes[5] = {
        -0.90617984594,
        -0.53846931010,
         0.0,
         0.53846931010,
         0.90617984594,
    };
    return nodes[min(index, 4u)];
}

float AreaLightGauss5Weight(uint index)
{
    static const float weights[5] = {
        0.23692688506,
        0.47862867050,
        0.56888888889,
        0.47862867050,
        0.23692688506,
    };
    return weights[min(index, 4u)];
}

uint AreaLightIntegrationSampleCount(LightingBuffer lighting)
{
    return lighting.areaLightSampleCount == 9u ? 9u :
           lighting.areaLightSampleCount == 64u ? 64u :
           AREA_LIGHT_INTEGRATION_SAMPLE_COUNT;
}

uint AreaLightQuadratureOrder(uint sampleCount)
{
    return sampleCount == 9u ? 3u : sampleCount == 64u ? 8u : 5u;
}

float AreaLightGaussNode(uint order, uint index)
{
    static const float nodes3[3] = {
        -0.774596669241483, 0.0, 0.774596669241483,
    };
    static const float nodes8[8] = {
        -0.960289856497536, -0.796666477413627,
        -0.525532409916329, -0.183434642495650,
         0.183434642495650,  0.525532409916329,
         0.796666477413627,  0.960289856497536,
    };
    if (order == 3u) return nodes3[min(index, 2u)];
    if (order == 8u) return nodes8[min(index, 7u)];
    return AreaLightGauss5Node(index);
}

float AreaLightGaussWeight(uint order, uint index)
{
    static const float weights3[3] = {
        0.555555555555556, 0.888888888888889, 0.555555555555556,
    };
    static const float weights8[8] = {
        0.101228536290376, 0.222381034453374,
        0.313706645877887, 0.362683783378362,
        0.362683783378362, 0.313706645877887,
        0.222381034453374, 0.101228536290376,
    };
    if (order == 3u) return weights3[min(index, 2u)];
    if (order == 8u) return weights8[min(index, 7u)];
    return AreaLightGauss5Weight(index);
}

float AreaLightRectSampleWeight(uint sampleIndex)
{
    uint index = sampleIndex % AREA_LIGHT_INTEGRATION_SAMPLE_COUNT;
    return 0.25 * AreaLightGauss5Weight(index % 5u) *
        AreaLightGauss5Weight(index / 5u);
}

float AreaLightSampleWeight(AreaLightFrame frame, uint sampleIndex)
{
    if (abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5)
    {
        return 0.1 * AreaLightGauss5Weight(
            (sampleIndex % AREA_LIGHT_INTEGRATION_SAMPLE_COUNT) / 5u);
    }
    return AreaLightRectSampleWeight(sampleIndex);
}

float3 AreaLightSamplePosition(AreaLightFrame frame, uint sampleIndex)
{
    float2 offset =
        (abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5)
            ? AreaLightDiskSampleOffset(sampleIndex) * frame.halfSize.x
            : AreaLightRectSampleOffset(sampleIndex) * frame.halfSize;
    return frame.position + frame.tangent * offset.x +
        frame.bitangent * offset.y;
}

float AreaLightPositionContributionWeight(AreaLightData light,
                                        AreaLightFrame frame,
                                        float3 worldPosition,
                                        float3 samplePosition, float sampleWeight,
                                        out float3 surfaceToLight)
{
    float3 lightVector = samplePosition - worldPosition;
    float distanceSq = dot(lightVector, lightVector);
    float range = light.positionRange.w;
    surfaceToLight = frame.normal;

    if (PointLightHasFiniteRange(range) && distanceSq >= range * range)
    {
        return 0.0;
    }

    surfaceToLight = lightVector * rsqrt(max(distanceSq, 1.0e-8));
    float sourceFacing = saturate(dot(frame.normal, -surfaceToLight));
    if (sourceFacing <= 0.0)
    {
        return 0.0;
    }

    return SmoothRangeAttenuation(distanceSq, range) * sourceFacing *
        frame.area * sampleWeight /
        max(distanceSq, AREA_LIGHT_MIN_DISTANCE_SQ);
}

float AreaLightSampleContributionWeight(AreaLightData light,
                                        AreaLightFrame frame,
                                        float3 worldPosition,
                                        uint sampleIndex,
                                        out float3 surfaceToLight)
{
    return AreaLightPositionContributionWeight(light, frame, worldPosition,
        AreaLightSamplePosition(frame, sampleIndex),
        AreaLightSampleWeight(frame, sampleIndex), surfaceToLight);
}

float3 EvaluateAreaLightSampleRadiance(AreaLightData light,
                                       AreaLightFrame frame,
                                       float3 worldPosition,
                                       uint sampleIndex,
                                       out float3 surfaceToLight)
{
    // This computes raw direct radiance. Callers apply local light-space
    // visibility separately.
    float contributionWeight = AreaLightSampleContributionWeight(
        light, frame, worldPosition, sampleIndex, surfaceToLight);
    return light.colorIntensity.rgb * light.colorIntensity.a *
        contributionWeight;
}

float3 EvaluateAreaLightSampleRadiance(AreaLightData light,
                                       AreaLightFrame frame,
                                       float3 worldPosition,
                                       uint sampleIndex, uint sampleCount,
                                       out float3 surfaceToLight)
{
    if (sampleCount == AREA_LIGHT_INTEGRATION_SAMPLE_COUNT)
        return EvaluateAreaLightSampleRadiance(
            light, frame, worldPosition, sampleIndex, surfaceToLight);

    uint order = AreaLightQuadratureOrder(sampleCount);
    uint x = sampleIndex % order;
    uint y = sampleIndex / order;
    float2 offset;
    float weight;
    if (abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5)
    {
        float radius = sqrt(0.5 * (AreaLightGaussNode(order, y) + 1.0));
        float angle = 2.0 * PI * float(x) / float(order);
        offset = radius * float2(cos(angle), sin(angle)) * frame.halfSize.x;
        weight = 0.5 * AreaLightGaussWeight(order, y) / float(order);
    }
    else
    {
        offset = float2(AreaLightGaussNode(order, x),
                        AreaLightGaussNode(order, y)) * frame.halfSize;
        weight = 0.25 * AreaLightGaussWeight(order, x) *
                        AreaLightGaussWeight(order, y);
    }
    float3 samplePosition = frame.position + frame.tangent * offset.x +
        frame.bitangent * offset.y;
    return light.colorIntensity.rgb * light.colorIntensity.a *
        AreaLightPositionContributionWeight(light, frame, worldPosition,
            samplePosition, weight, surfaceToLight);
}
