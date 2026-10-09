#include "lighting_structs.slang"
#include "push_constants_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(0, 1)]] Texture2DArray<float4> uLightGizmoIcons;
[[vk::binding(1, 1)]] SamplerState uLightGizmoSampler;

[[vk::push_constant]]
ConstantBuffer<LightPushConstants> pc;

static const uint kCoveragePoint = 1u;
static const uint kCoverageSpot = 2u;
static const uint kCoverageArea = 3u;
static const uint kCoverageDirectional = 4u;
static const uint kCoverageRingSegments = 32u;
static const uint kCoveragePointVertexCount = kCoverageRingSegments * 6u;
static const uint kCoverageSpotVertexCount = kCoverageRingSegments * 2u + 8u;
static const uint kCoverageAreaVertexCount = 14u;
static const uint kCoverageDirectionalVertexCount = 10u;
static const float kTau = 6.28318530718;

struct VSOutput
{
    float4 pos : SV_Position;
    float3 color : COLOR0;
    float alpha : COLOR1;
    float2 uv : TEXCOORD0;
    float iconLayer : TEXCOORD1;
    float coverageMode : TEXCOORD2;
};

float3 safeNormalize(float3 value, float3 fallback)
{
    float len2 = dot(value, value);
    return len2 > 0.0001 ? value * rsqrt(len2) : fallback;
}

float3 tangentFromDirection(float3 direction)
{
    float3 upSeed = abs(dot(direction, float3(0.0, 1.0, 0.0))) > 0.95
        ? float3(1.0, 0.0, 0.0)
        : float3(0.0, 1.0, 0.0);
    return safeNormalize(cross(upSeed, direction), float3(1.0, 0.0, 0.0));
}

float3 coverageBasisTangent(float3 direction)
{
    float3 tangent = pc.coneOuterCosType.xyz;
    tangent = tangent - direction * dot(tangent, direction);
    return safeNormalize(tangent, tangentFromDirection(direction));
}

float3 pointCoveragePosition(uint vertexIndex)
{
    if (vertexIndex >= kCoveragePointVertexCount)
    {
        return float3(0.0, 0.0, 0.0);
    }

    uint verticesPerRing = kCoverageRingSegments * 2u;
    uint ring = vertexIndex / verticesPerRing;
    uint pairVertex = vertexIndex % verticesPerRing;
    uint segment = pairVertex / 2u;
    uint endpoint = pairVertex & 1u;
    float angle = (float(segment + endpoint) / float(kCoverageRingSegments)) * kTau;
    float s = sin(angle);
    float c = cos(angle);
    float radius = max(pc.positionRadius.w, 0.0);
    float3 offset = ring == 0u
        ? float3(c, s, 0.0)
        : (ring == 1u ? float3(c, 0.0, s) : float3(0.0, c, s));
    return pc.positionRadius.xyz + offset * radius;
}

float3 spotCoveragePosition(uint vertexIndex)
{
    if (vertexIndex >= kCoverageSpotVertexCount)
    {
        return float3(0.0, 0.0, 0.0);
    }

    float3 direction = safeNormalize(pc.directionInnerCos.xyz, float3(0.0, -1.0, 0.0));
    float3 tangent = coverageBasisTangent(direction);
    float3 bitangent = safeNormalize(cross(direction, tangent), float3(0.0, 0.0, 1.0));
    float range = max(pc.positionRadius.w, 0.0);
    float coneRadius = max(pc.coneOuterCosType.w, 0.0);
    float3 apex = pc.positionRadius.xyz;
    float3 baseCenter = apex + direction * range;

    uint ringVertices = kCoverageRingSegments * 2u;
    if (vertexIndex < ringVertices)
    {
        uint segment = vertexIndex / 2u;
        uint endpoint = vertexIndex & 1u;
        float angle = (float(segment + endpoint) / float(kCoverageRingSegments)) * kTau;
        return baseCenter + (tangent * cos(angle) + bitangent * sin(angle)) * coneRadius;
    }

    uint edgeVertex = vertexIndex - ringVertices;
    uint edge = edgeVertex / 2u;
    uint endpoint = edgeVertex & 1u;
    if (endpoint == 0u)
    {
        return apex;
    }
    float edgeAngle = (float(edge) / 4.0) * kTau;
    return baseCenter + (tangent * cos(edgeAngle) + bitangent * sin(edgeAngle)) * coneRadius;
}

float3 areaCoverageCorner(float3 center, float3 tangent, float3 bitangent,
                          float halfX, float halfY, uint corner)
{
    float sx = (corner == 0u || corner == 3u) ? -1.0 : 1.0;
    float sy = (corner == 0u || corner == 1u) ? -1.0 : 1.0;
    return center + tangent * (sx * halfX) + bitangent * (sy * halfY);
}

float3 areaCoveragePosition(uint vertexIndex)
{
    bool disk = abs(pc.bounceIntensity - AREA_LIGHT_TYPE_DISK) < 0.5;
    uint boundarySegments = disk ? kCoverageRingSegments : 4u;
    uint vertexCount = disk ? (boundarySegments + 3u) * 2u : kCoverageAreaVertexCount;
    if (vertexIndex >= vertexCount)
    {
        return float3(0.0, 0.0, 0.0);
    }

    float3 normal = safeNormalize(pc.directionInnerCos.xyz, float3(0.0, -1.0, 0.0));
    float3 tangent = coverageBasisTangent(normal);
    float3 bitangent = safeNormalize(cross(normal, tangent), float3(0.0, 0.0, 1.0));
    float halfX = max(pc.directionInnerCos.w, 0.0);
    float halfY = max(pc.coneOuterCosType.w, 0.0);
    uint segment = vertexIndex / 2u;
    uint endpoint = vertexIndex & 1u;

    if (disk && segment < boundarySegments)
    {
        float angle = float(segment + endpoint) * kTau / float(boundarySegments);
        return pc.positionRadius.xyz +
            (tangent * cos(angle) + bitangent * sin(angle)) * halfX;
    }
    if (!disk && segment < boundarySegments)
    {
        uint corner = endpoint == 0u ? segment : ((segment + 1u) % 4u);
        return areaCoverageCorner(pc.positionRadius.xyz, tangent, bitangent,
                                  halfX, halfY, corner);
    }
    if (segment == boundarySegments)
    {
        return pc.positionRadius.xyz + tangent * (endpoint == 0u ? -halfX : halfX);
    }
    if (segment == boundarySegments + 1u)
        return pc.positionRadius.xyz + bitangent * (endpoint == 0u ? -halfY : halfY);
    // The normal marks the emitting side; positions remain in the shared world
    // convention and the existing negative-height scene viewport handles Y.
    return pc.positionRadius.xyz + normal *
        (endpoint == 0u ? 0.0 : clamp(max(halfX, halfY) * 0.35, 0.02, 0.5));
}

float3 directionalCoveragePosition(uint vertexIndex)
{
    if (vertexIndex >= kCoverageDirectionalVertexCount)
    {
        return float3(0.0, 0.0, 0.0);
    }

    float3 direction = safeNormalize(pc.directionInnerCos.xyz, float3(0.0, -1.0, 0.0));
    float3 tangent = coverageBasisTangent(direction);
    float3 bitangent = safeNormalize(cross(direction, tangent), float3(0.0, 0.0, 1.0));
    float length = max(pc.positionRadius.w, 0.0);
    float headWidth = max(pc.directionInnerCos.w, 0.0);
    float headLength = max(pc.coneOuterCosType.w, 0.0);
    float3 base = pc.positionRadius.xyz;
    float3 tip = base + direction * length;

    if (vertexIndex < 2u)
    {
        return vertexIndex == 0u ? base : tip;
    }

    uint headVertex = vertexIndex - 2u;
    uint headSegment = headVertex / 2u;
    uint endpoint = headVertex & 1u;
    if (endpoint == 0u)
    {
        return tip;
    }

    float3 neck = tip - direction * headLength;
    if (headSegment == 0u)
    {
        return neck + tangent * headWidth;
    }
    if (headSegment == 1u)
    {
        return neck - tangent * headWidth;
    }
    if (headSegment == 2u)
    {
        return neck + bitangent * headWidth;
    }
    return neck - bitangent * headWidth;
}

float3 coverageLinePosition(uint vertexIndex)
{
    if (pc.localShadowEnabled == kCoveragePoint)
    {
        return pointCoveragePosition(vertexIndex);
    }
    if (pc.localShadowEnabled == kCoverageSpot)
    {
        return spotCoveragePosition(vertexIndex);
    }
    if (pc.localShadowEnabled == kCoverageArea)
    {
        return areaCoveragePosition(vertexIndex);
    }
    if (pc.localShadowEnabled == kCoverageDirectional)
    {
        return directionalCoveragePosition(vertexIndex);
    }
    return float3(0.0, 0.0, 0.0);
}

[shader("vertex")]
VSOutput vertMain(uint vertexIndex : SV_VertexID)
{
    static const float2 kQuadCorners[6] = {
        float2(-1.0, -1.0),
        float2( 1.0, -1.0),
        float2( 1.0,  1.0),
        float2(-1.0, -1.0),
        float2( 1.0,  1.0),
        float2(-1.0,  1.0),
    };
    static const float2 kQuadUvs[6] = {
        float2(0.0, 1.0),
        float2(1.0, 1.0),
        float2(1.0, 0.0),
        float2(0.0, 1.0),
        float2(1.0, 0.0),
        float2(0.0, 0.0),
    };

    VSOutput output;
    output.alpha = pc.colorIntensity.a;
    output.coverageMode = pc.contactVisibilityEnabled != 0u ? 1.0 : 0.0;
    if (pc.contactVisibilityEnabled != 0u)
    {
        float3 worldPosition = coverageLinePosition(vertexIndex);
        output.pos = mul(uCamera.temporalInfo.w != 0u ? uCamera.unjitteredViewProj : uCamera.viewProj, float4(worldPosition, 1.0));
        output.color = pc.colorIntensity.rgb;
        output.uv = float2(0.0, 0.0);
        output.iconLayer = 0.0;
        return output;
    }

    float gizmoRadius =
        PointLightHasFiniteRange(pc.positionRadius.w) ? pc.positionRadius.w : 1.0;
    float3 cameraForwardRaw = uCamera.cameraForward.xyz;
    float cameraForwardLen2 = dot(cameraForwardRaw, cameraForwardRaw);
    float3 cameraForward = cameraForwardLen2 > 0.0001
        ? cameraForwardRaw * rsqrt(cameraForwardLen2)
        : float3(0.0, 0.0, -1.0);
    float3 upSeed = abs(dot(cameraForward, float3(0.0, 1.0, 0.0))) > 0.95
        ? float3(1.0, 0.0, 0.0)
        : float3(0.0, 1.0, 0.0);
    float3 right = normalize(cross(upSeed, cameraForward));
    float3 up = normalize(cross(cameraForward, right));
    float2 corner = kQuadCorners[vertexIndex];
    float3 worldPosition =
        pc.positionRadius.xyz + (right * corner.x + up * corner.y) * gizmoRadius;
    output.pos = mul(uCamera.temporalInfo.w != 0u ? uCamera.unjitteredViewProj : uCamera.viewProj, float4(worldPosition, 1.0));
    output.color = pc.colorIntensity.rgb;
    output.alpha = 1.0;
    output.uv = kQuadUvs[vertexIndex];
    output.iconLayer = max(pc.coneOuterCosType.x, 0.0);
    return output;
}

float4 unlitGizmoColor(VSOutput input)
{
    if (input.coverageMode > 0.5)
    {
        return float4(input.color, input.alpha);
    }
    float layer = floor(input.iconLayer + 0.5);
    float4 icon = uLightGizmoIcons.Sample(uLightGizmoSampler, float3(input.uv, layer));
    return float4(input.color * icon.rgb, icon.a);
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    return unlitGizmoColor(input);
}
