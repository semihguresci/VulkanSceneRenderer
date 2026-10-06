#pragma once

struct BindlessPushConstants
{
    uint objectIndex;
    uint sectionPlaneEnabled;
    uint semanticColorMode;
    uint padding0;
    float4 sectionPlane;
};

struct LightPushConstants
{
    float4 positionRadius;
    float4 colorIntensity;
    float4 directionInnerCos;
    float4 coneOuterCosType;
    uint contactVisibilityEnabled;
    uint localShadowEnabled;
    float bounceIntensity;
    uint padding2; // point-light SSBO index, or gizmo pick ID
};

struct ShadowPushConstants
{
    uint objectIndex;
    uint cascadeIndex;
    uint sectionPlaneEnabled;
    uint padding0;
    float4 sectionPlane;
};

bool SectionPlaneClips(float3 worldPos, uint enabled, float4 plane)
{
    return enabled != 0u && dot(plane.xyz, worldPos) + plane.w < 0.0;
}

static const uint SECTION_CLIP_CAP_HATCH_NONE = 0u;
static const uint SECTION_CLIP_CAP_HATCH_DIAGONAL = 1u;
static const uint SECTION_CLIP_CAP_HATCH_CROSS = 2u;

struct BoxClipState
{
    uint enabled;
    uint invert;
    uint planeCount;
    uint padding0;
    float4 planes[6];
};

struct SectionClipCapStyle
{
    uint enabled;
    uint fillEnabled;
    uint hatchEnabled;
    uint hatchMode;
    float4 fillColor;
    float4 hatchColor;
    float hatchSpacing;
    float hatchLineWidth;
    float hatchAngleRadians;
    float capOffset;
    BoxClipState boxClip;
};

bool BoxClipPlanesClip(float3 worldPos, BoxClipState boxClip)
{
    if (boxClip.enabled == 0u)
    {
        return false;
    }

    const uint count = min(boxClip.planeCount, 6u);
    bool outside = false;
    for (uint i = 0u; i < count; ++i)
    {
        outside = outside ||
            dot(boxClip.planes[i].xyz, worldPos) + boxClip.planes[i].w < 0.0;
    }
    return boxClip.invert != 0u ? !outside : outside;
}

float SectionClipCapHatchMask(float3 worldPos, SectionClipCapStyle style)
{
    if (style.enabled == 0u ||
        style.hatchEnabled == 0u ||
        style.hatchMode == SECTION_CLIP_CAP_HATCH_NONE)
    {
        return 0.0;
    }

    const float spacing = max(style.hatchSpacing, 0.0001);
    const float width = max(style.hatchLineWidth, 0.0001);
    const float2 direction =
        float2(cos(style.hatchAngleRadians), sin(style.hatchAngleRadians));
    const float diagonalPhase =
        abs(frac(dot(worldPos.xz, direction) / spacing) - 0.5) * spacing;
    float mask = diagonalPhase <= width ? 1.0 : 0.0;

    if (style.hatchMode == SECTION_CLIP_CAP_HATCH_CROSS)
    {
        const float2 crossDirection = float2(-direction.y, direction.x);
        const float crossPhase =
            abs(frac(dot(worldPos.xz, crossDirection) / spacing) - 0.5) *
            spacing;
        mask = max(mask, crossPhase <= width ? 1.0 : 0.0);
    }

    return mask;
}

struct PostProcessPushConstants
{
    uint outputMode;
    uint bloomEnabled;
    float bloomIntensity;
    float exposure;
    float cameraNear;
    float cameraFar;
    float cascadeSplits[4];
    uint tileCountX;
    uint totalLights;
    uint depthSliceCount;
    uint oitEnabled;
    uint exposureMode;
    float targetLuminance;
    float minExposure;
    float maxExposure;
    float adaptationRate;
};

struct TileCullPushConstants
{
    uint tileCountX;
    uint tileCountY;
    uint depthSliceCount;
    uint totalLights;
    float cameraNear;
    float cameraFar;
};

struct TiledLightingPushConstants
{
    uint tileCountX;
    uint tileCountY;
    uint depthSliceCount;
    float cameraNear;
    float cameraFar;
    uint contactVisibilityEnabled;
    uint localShadowEnabled;
    float bounceIntensity;
};

struct CullPushConstants
{
    uint objectCount;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct HiZPushConstants
{
    uint srcWidth;
    uint srcHeight;
    uint dstMipLevel;
    uint pad0;
};

struct WireframePushConstants
{
    uint objectIndex;
    uint sectionPlaneEnabled;
    uint useObjectColor;
    uint padding1;
    float4 sectionPlane;
    float4 colorIntensity;
    float lineWidth;
    float padding7;
    float padding8;
    float padding9;
};

struct NormalValidationPushConstants
{
    uint objectIndex;
    uint showFaceFill;
    float faceAlpha;
    uint faceClassificationFlags;
    uint sectionPlaneEnabled;
    uint padding0;
    uint padding1;
    uint padding2;
    float4 sectionPlane;
};

struct SurfaceNormalPushConstants
{
    uint objectIndex;
    float lineLength;
    float lineOffset;
    uint sectionPlaneEnabled;
    float4 sectionPlane;
};

struct TransformGizmoPushConstants
{
    float4 originScale;
    float4 axisX;
    float4 axisY;
    float4 axisZ;
    uint tool;
    uint activeAxis;
    uint transformSpace;
    uint padding0;
};

struct GBufferCompositePushConstants
{
    uint outputMode;
};

struct BloomDownsamplePushConstants
{
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
    float threshold;
    float knee;
    uint mipLevel;
    uint pad0;
};

struct BloomUpsamplePushConstants
{
    uint srcWidth;
    uint srcHeight;
    uint dstWidth;
    uint dstHeight;
    float filterRadius;
    float bloomIntensity;
    uint isFinalPass;
    uint pad0;
};

struct ExposureHistogramPushConstants
{
    uint width;
    uint height;
    uint binCount;
    float minLogLuminance;
    float maxLogLuminance;
    float pad0;
    float pad1;
    float pad2;
};

struct ExposureAdaptPushConstants
{
    uint binCount;
    uint exposureMode;
    float minLogLuminance;
    float maxLogLuminance;
    float targetLuminance;
    float minExposure;
    float maxExposure;
    float adaptationRate;
    float meteringLowPercentile;
    float meteringHighPercentile;
    float deltaSeconds;
    float manualExposure;
    float pad0;
    float pad1;
    float pad2;
    float pad3;
};

struct EquirectPushConstants
{
    uint faceIndex;
    uint faceSize;
};

struct IrradiancePushConstants
{
    uint faceIndex;
    uint faceSize;
};

struct PrefilterPushConstants
{
    uint faceIndex;
    uint faceSize;
    float roughness;
    uint sourceMipCount;
};

struct GtaoPushConstants
{
    float aoRadius;
    float aoIntensity;
    uint sampleCount;
    uint pad0;
    uint fullWidth;
    uint fullHeight;
    uint pad1;
    uint pad2;
};

struct BlurPushConstants
{
    uint width;
    uint height;
    float depthThreshold;
    float cameraNear;
    float cameraFar;
    uint orthographicDepth;
    float pad1;
    float pad2;
};
