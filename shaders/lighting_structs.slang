// lighting_structs.slang — Shared lighting / camera struct definitions.
// Must match the C++ LightingData and CameraData layouts exactly.
#pragma once

static const uint MAX_POINT_LIGHTS = 12u;
static const uint MAX_CLUSTERED_LIGHTS = 8192u;
static const uint MAX_AREA_LIGHTS = 256u;
static const uint TILE_SIZE = 16u;
static const uint CLUSTER_DEPTH_SLICES = 16u;
static const uint MAX_LIGHTS_PER_TILE = 128u;
static const uint LOCAL_SHADOW_MAP_RESOLUTION = 2048u;
static const uint MAX_SHADOWED_LOCAL_LIGHT_LAYERS = 24u;
static const uint LOCAL_SHADOW_POINT_FACE_COUNT = 6u;
static const uint LOCAL_SHADOW_SPOT_LAYER_COUNT = 1u;
static const uint AREA_LIGHT_SHADOW_VISIBILITY_SAMPLE_COUNT = 4u;
static const uint LOCAL_SHADOW_AREA_LAYER_COUNT = LOCAL_SHADOW_POINT_FACE_COUNT;
static const uint LOCAL_SHADOW_AREA_REF_PACKED_COUNT = 64u;
static const float UNBOUNDED_POINT_LIGHT_RANGE = 0.0;
static const float LOCAL_SHADOW_TYPE_POINT = 0.0;
static const float LOCAL_SHADOW_TYPE_SPOT = 1.0;
static const float LOCAL_SHADOW_TYPE_AREA = 2.0;

// These structs mirror C++ GPU upload structs in SceneData.h. Keep float4
// alignment, array sizes, and field order synchronized with the static_asserts.
struct CameraBuffer
{
    column_major float4x4 viewProj;
    column_major float4x4 inverseViewProj;
    float4 cameraWorldPosition;
    float4 cameraForward;
    column_major float4x4 unjitteredViewProj;
    column_major float4x4 previousViewProj;
    column_major float4x4 previousJitteredViewProj;
    column_major float4x4 previousInverseViewProj;
    float4 jitterUv;
    uint4 temporalInfo;
    float4 renderExtent;
};

struct PointLightData
{
    // positionRadius.w > 0 is the finite fade/cull range; 0 means unbounded.
    // colorIntensity.a is scene-linear point/spot-light intensity.
    float4 positionRadius;
    float4 colorIntensity;
    float4 directionInnerCos;
    float4 coneOuterCosType;
};

static const float AREA_LIGHT_TYPE_RECTANGLE = 0.0;
static const float AREA_LIGHT_TYPE_DISK = 1.0;

struct AreaLightData
{
    float4 positionRange;
    float4 colorIntensity;
    float4 directionType;
    float4 tangentHalfSize;
    float4 bitangentHalfSize;
};

bool PointLightHasFiniteRange(float radius)
{
    return isfinite(radius) && radius > UNBOUNDED_POINT_LIGHT_RANGE;
}

bool PointLightHasRenderableIntensity(PointLightData light)
{
    return all(isfinite(light.colorIntensity)) &&
           light.colorIntensity.a > 0.0 &&
           any(light.colorIntensity.rgb > 0.0.xxx);
}

static const float LIGHT_TYPE_SPOT = 1.0;

float3 SafeNormalizeLightDirection(float3 value, float3 fallback)
{
    float lengthSq = dot(value, value);
    if (!all(isfinite(value)) || lengthSq < 1e-8)
    {
        return fallback;
    }
    return value * rsqrt(lengthSq);
}

float SpotLightConeAttenuation(float4 directionInnerCos,
                               float4 coneOuterCosType,
                               float3 surfaceToLight)
{
    if (coneOuterCosType.y < 0.5)
    {
        return 1.0;
    }

    float3 spotDirection =
        SafeNormalizeLightDirection(directionInnerCos.xyz,
                                    float3(0.0, 0.0, -1.0));
    float innerCos = saturate(directionInnerCos.w);
    float outerCos = saturate(coneOuterCosType.x);
    float cosTheta = dot(spotDirection, -surfaceToLight);
    float cone = saturate((cosTheta - outerCos) / max(innerCos - outerCos, 1e-4));
    return cone * cone;
}

struct LightingBuffer
{
    float4 directionalDirection;
    float4 directionalColorIntensity;
    uint pointLightCount;
    uint shadowEnabled;
    uint gtaoEnabled;
    uint tileCullEnabled;
    uint prefilteredMipCount;
    float environmentIntensity;
    uint areaLightCount;
    uint localShadowEnabled;
    float bounceIntensity;
    uint localContactVisibility;
    uint padding1;
    uint padding2;
};

struct ExposureStateData
{
    float exposure;
    float averageLuminance;
    float targetExposure;
    float initialized;
};

struct TileLightGrid
{
    uint offset;
    uint count;
};

float SanitizeClusterNear(float nearPlane)
{
    return isfinite(nearPlane) ? max(nearPlane, 1e-4) : 0.1;
}

float SanitizeClusterFar(float nearPlane, float farPlane)
{
    float n = SanitizeClusterNear(nearPlane);
    float f = isfinite(farPlane) ? farPlane : 100.0;
    return max(f, n + 1e-3);
}

float LinearizeReverseZDepth(float depth, float nearPlane, float farPlane)
{
    // Reverse-Z perspective depth maps near -> 1 and far -> 0. The linearized
    // value returned here is a positive view-space distance from the camera.
    float n = SanitizeClusterNear(nearPlane);
    float f = SanitizeClusterFar(n, farPlane);
    return (n * f) / (saturate(depth) * (f - n) + n);
}

float ViewDepthToReverseZ(float viewDepth, float nearPlane, float farPlane)
{
    float n = SanitizeClusterNear(nearPlane);
    float f = SanitizeClusterFar(n, farPlane);
    float z = clamp(viewDepth, n, f);
    return saturate(n * (f / z - 1.0) / (f - n));
}

uint DepthToClusterSlice(float depth, float nearPlane, float farPlane, uint depthSliceCount)
{
    // Logarithmic slicing gives more resolution near the camera where point
    // lights change screen coverage fastest.
    uint safeSliceCount = max(depthSliceCount, 1u);
    float n = SanitizeClusterNear(nearPlane);
    float f = SanitizeClusterFar(n, farPlane);
    float viewDepth = clamp(LinearizeReverseZDepth(depth, n, f), n, f);
    float logRange = max(log2(f / n), 1e-6);
    float normalized = log2(viewDepth / n) / logRange;
    uint slice = uint(saturate(normalized) * float(safeSliceCount));
    return min(slice, safeSliceCount - 1u);
}

float ClusterSliceBoundaryDepth(uint sliceBoundary, float nearPlane, float farPlane, uint depthSliceCount)
{
    uint safeSliceCount = max(depthSliceCount, 1u);
    float n = SanitizeClusterNear(nearPlane);
    float f = SanitizeClusterFar(n, farPlane);
    uint safeBoundary = min(sliceBoundary, safeSliceCount);
    float t = float(safeBoundary) / float(safeSliceCount);
    float viewDepth = n * exp2(log2(f / n) * t);
    return ViewDepthToReverseZ(viewDepth, n, f);
}

static const uint SHADOW_CASCADE_COUNT = 4u;

struct ShadowCascadeData
{
    column_major float4x4 viewProj;
    float splitDepth;
    float texelSize;
    float worldRadius;
    float depthRange;
};

struct ShadowBuffer
{
    ShadowCascadeData cascades[SHADOW_CASCADE_COUNT];
    // x = normal-bias min texels, y = normal-bias max texels,
    // z = slope depth-bias scale, w = receiver-plane depth-bias scale.
    float4 biasSettings;
    // x = PCF radius in texels, y = cascade blend fraction,
    // z = constant depth bias, w = max depth bias.
    float4 filterSettings;
    // x = directional PCSS enabled, y = tan(light angular radius),
    // z = blocker-search radius in texels, w = max adaptive filter radius.
    float4 softShadowSettings;
    // x = directional contact enabled, y = max ray distance, z = ray thickness,
    // w = fade distance; distances are in world units.
    float4 contactShadowSettings;
};

struct LocalShadowLayerData
{
    column_major float4x4 viewProj;
    float4 positionRange;
    float4 directionType;
    uint4 meta;
    // x = texel size, y = depth range, z = outer cone cosine,
    // w = source radius used to widen soft local-shadow filtering.
    float4 params;
};

struct LocalShadowBuffer
{
    LocalShadowLayerData layers[MAX_SHADOWED_LOCAL_LIGHT_LAYERS];
    uint4 areaLightRefs[LOCAL_SHADOW_AREA_REF_PACKED_COUNT];
    uint4 counts;
    float4 biasSettings;
    float4 filterSettings;
};

struct ShadowCascadeCullData
{
    column_major float4x4 viewProj;
    column_major float4x4 lightView;
    float4 receiverMinBounds;
    float4 receiverMaxBounds;
    float4 casterMinBounds;
    float4 casterMaxBounds;
};

struct ShadowCullBuffer
{
    ShadowCascadeCullData cascades[SHADOW_CASCADE_COUNT];
};

struct ShadowCullPushConstants
{
    uint drawCount;
    uint cascadeIndex;
    uint outputOffset;
    uint objectCount;
};
