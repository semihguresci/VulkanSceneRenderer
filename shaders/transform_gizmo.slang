#include "lighting_structs.slang"
#include "push_constants_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;

[[vk::push_constant]]
ConstantBuffer<TransformGizmoPushConstants> pc;

static const uint kToolRotate = 2;
static const uint kToolScale = 3;
static const uint kGeometrySolidArrowheads = 1;
static const uint kAxisFree = 0;
static const uint kAxisX = 1;
static const uint kAxisY = 2;
static const uint kAxisZ = 3;
static const uint kAxisLineVertexCount = 2;
static const uint kScaleLineVertexCount = 26;
static const uint kViewBoxVertexCount = 24;
static const uint kRingSegments = 64;
static const float kPi = 3.14159265358979323846;

struct VSOutput
{
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

float3 safeNormalize(float3 value, float3 fallback)
{
    const float lenSq = dot(value, value);
    return lenSq > 1.0e-8 ? value * rsqrt(lenSq) : fallback;
}

float3 axisDirection(uint axis)
{
    if (axis == kAxisX)
        return safeNormalize(pc.axisX.xyz, float3(1.0, 0.0, 0.0));
    if (axis == kAxisY)
        return safeNormalize(pc.axisY.xyz, float3(0.0, 1.0, 0.0));
    return safeNormalize(pc.axisZ.xyz, float3(0.0, 0.0, 1.0));
}

float3 axisBaseColor(uint axis)
{
    if (axis == kAxisX)
        return float3(1.0, 0.18, 0.16);
    if (axis == kAxisY)
        return float3(0.22, 0.88, 0.26);
    return float3(0.18, 0.42, 1.0);
}

float4 axisColor(uint axis)
{
    if (axis == kAxisFree)
        return float4(0.78, 0.82, 0.84, 0.58);

    float3 color = axisBaseColor(axis);
    if (pc.activeAxis != kAxisFree && pc.activeAxis != axis)
        color *= 0.38;
    if (pc.activeAxis == axis)
        color = lerp(color, float3(1.0, 0.92, 0.16), 0.55);
    return float4(color, 0.96);
}

void axisSideBasis(uint axis, out float3 sideA, out float3 sideB)
{
    if (axis == kAxisX)
    {
        sideA = axisDirection(kAxisY);
        sideB = axisDirection(kAxisZ);
    }
    else if (axis == kAxisY)
    {
        sideA = axisDirection(kAxisX);
        sideB = axisDirection(kAxisZ);
    }
    else
    {
        sideA = axisDirection(kAxisX);
        sideB = axisDirection(kAxisY);
    }
}

uint boxCornerIndex(uint edge, uint endpoint)
{
    if (edge == 0)
        return endpoint == 0 ? 0 : 1;
    if (edge == 1)
        return endpoint == 0 ? 2 : 3;
    if (edge == 2)
        return endpoint == 0 ? 4 : 5;
    if (edge == 3)
        return endpoint == 0 ? 6 : 7;
    if (edge == 4)
        return endpoint == 0 ? 0 : 2;
    if (edge == 5)
        return endpoint == 0 ? 1 : 3;
    if (edge == 6)
        return endpoint == 0 ? 4 : 6;
    if (edge == 7)
        return endpoint == 0 ? 5 : 7;
    if (edge == 8)
        return endpoint == 0 ? 0 : 4;
    if (edge == 9)
        return endpoint == 0 ? 1 : 5;
    if (edge == 10)
        return endpoint == 0 ? 2 : 6;
    return endpoint == 0 ? 3 : 7;
}

float3 boxCorner(float3 center,
                 float3 axisA,
                 float3 axisB,
                 float3 axisC,
                 float halfExtent,
                 uint cornerIndex)
{
    const float signA = (cornerIndex & 1) != 0 ? 1.0 : -1.0;
    const float signB = (cornerIndex & 2) != 0 ? 1.0 : -1.0;
    const float signC = (cornerIndex & 4) != 0 ? 1.0 : -1.0;
    return center + axisA * signA * halfExtent +
           axisB * signB * halfExtent +
           axisC * signC * halfExtent;
}

float3 axisLinePosition(uint axis, uint localVertex)
{
    const float3 origin = pc.originScale.xyz;
    const float scale = max(pc.originScale.w, 0.0001);
    const float3 dir = axisDirection(axis);
    const float headLength = scale * 0.18;
    const float3 headBase = origin + dir * (scale - headLength);
    return localVertex == 0 ? origin : headBase;
}

float3 axisBillboardSide(uint axis, float3 dir, float3 anchor)
{
    float3 sideA;
    float3 sideB;
    axisSideBasis(axis, sideA, sideB);
    const float3 viewDir = safeNormalize(
        uCamera.cameraWorldPosition.xyz - anchor, -uCamera.cameraForward.xyz);
    return safeNormalize(cross(viewDir, dir), sideA);
}

float3 axisHeadTrianglePosition(uint axis, uint localVertex)
{
    const float3 origin = pc.originScale.xyz;
    const float scale = max(pc.originScale.w, 0.0001);
    const float3 dir = axisDirection(axis);
    const float headLength = scale * 0.18;
    const float headWidth = scale * 0.075;
    const float3 tip = origin + dir * scale;
    const float3 headBase = tip - dir * headLength;
    const float3 side = axisBillboardSide(axis, dir, headBase);

    if (localVertex == 0)
        return tip;
    if (localVertex == 1)
        return headBase + side * headWidth;
    return headBase - side * headWidth;
}

float3 viewBoxPosition(uint localVertex)
{
    const float scale = max(pc.originScale.w, 0.0001);
    const float halfExtent = scale * 0.12;
    const uint edge = localVertex / 2;
    const uint endpoint = localVertex & 1;
    const uint cornerIndex = boxCornerIndex(edge, endpoint);
    return boxCorner(pc.originScale.xyz,
                     axisDirection(kAxisX),
                     axisDirection(kAxisY),
                     axisDirection(kAxisZ),
                     halfExtent,
                     cornerIndex);
}

uint scaleBoxCornerIndex(uint edge, uint endpoint)
{
    return boxCornerIndex(edge, endpoint);
}

float3 scaleBoxCorner(float3 center,
                      float3 dir,
                      float3 sideA,
                      float3 sideB,
                      float halfExtent,
                      uint cornerIndex)
{
    return boxCorner(center, dir, sideA, sideB, halfExtent, cornerIndex);
}

float3 scaleLinePosition(uint axis, uint localVertex)
{
    const float3 origin = pc.originScale.xyz;
    const float scale = max(pc.originScale.w, 0.0001);
    const float3 dir = axisDirection(axis);
    float3 sideA;
    float3 sideB;
    axisSideBasis(axis, sideA, sideB);

    const float halfExtent = scale * 0.085;
    const float3 boxCenter = origin + dir * scale * 0.92;
    const float3 boxNearFace = boxCenter - dir * halfExtent;
    if (localVertex == 0)
        return origin;
    if (localVertex == 1)
        return boxNearFace;

    const uint edgeVertex = localVertex - 2;
    const uint edge = edgeVertex / 2;
    const uint endpoint = edgeVertex & 1;
    const uint cornerIndex = scaleBoxCornerIndex(edge, endpoint);
    return scaleBoxCorner(boxCenter, dir, sideA, sideB, halfExtent,
                          cornerIndex);
}

float3 ringPosition(uint axis, uint localVertex)
{
    const uint segment = localVertex / 2;
    const uint endpoint = localVertex & 1;
    const float angle =
        (float(segment) + float(endpoint)) *
        (2.0 * kPi / float(kRingSegments));

    float3 sideA;
    float3 sideB;
    axisSideBasis(axis, sideA, sideB);
    return pc.originScale.xyz +
           (cos(angle) * sideA + sin(angle) * sideB) *
               max(pc.originScale.w, 0.0001);
}

uint toolMainVertexCount()
{
    if (pc.tool == kToolRotate)
        return 3 * kRingSegments * 2;
    if (pc.tool == kToolScale)
        return 3 * kScaleLineVertexCount;
    return 3 * kAxisLineVertexCount;
}

[shader("vertex")]
VSOutput vertMain(uint vertexIndex : SV_VertexID)
{
    VSOutput output;
    uint axis = kAxisX;
    float3 worldPosition = pc.originScale.xyz;

    if (pc.padding0 == kGeometrySolidArrowheads)
    {
        axis = vertexIndex / 3 + 1;
        const uint localVertex = vertexIndex % 3;
        worldPosition = axisHeadTrianglePosition(axis, localVertex);
        output.pos = mul(uCamera.temporalInfo.w != 0u ? uCamera.unjitteredViewProj : uCamera.viewProj, float4(worldPosition, 1.0));
        output.color = axisColor(axis);
        return output;
    }

    const uint mainVertexCount = toolMainVertexCount();
    if (vertexIndex >= mainVertexCount)
    {
        axis = kAxisFree;
        worldPosition = viewBoxPosition(vertexIndex - mainVertexCount);
    }
    else if (pc.tool == kToolRotate)
    {
        const uint ringVertexCount = kRingSegments * 2;
        axis = vertexIndex / ringVertexCount + 1;
        const uint localVertex = vertexIndex % ringVertexCount;
        worldPosition = ringPosition(axis, localVertex);
    }
    else if (pc.tool == kToolScale)
    {
        axis = vertexIndex / kScaleLineVertexCount + 1;
        const uint localVertex = vertexIndex % kScaleLineVertexCount;
        worldPosition = scaleLinePosition(axis, localVertex);
    }
    else
    {
        axis = vertexIndex / kAxisLineVertexCount + 1;
        const uint localVertex = vertexIndex % kAxisLineVertexCount;
        worldPosition = axisLinePosition(axis, localVertex);
    }

    output.pos = mul(uCamera.temporalInfo.w != 0u ? uCamera.unjitteredViewProj : uCamera.viewProj, float4(worldPosition, 1.0));
    output.color = axisColor(axis);
    return output;
}

[shader("fragment")]
float4 fragMain(VSOutput input) : SV_Target0
{
    return input.color;
}
