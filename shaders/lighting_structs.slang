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
    float4 iesTangent;
    float* iesProfile;
    uint2 iesPadding;
};

static const float AREA_LIGHT_TYPE_RECTANGLE = 0.0;
static const float AREA_LIGHT_TYPE_DISK = 1.0;
static const uint AREA_LIGHTING_MODE_SAMPLED = 0u;
static const uint AREA_LIGHTING_MODE_LTC = 1u;

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

struct IesInterval
{
    uint low;
    uint high;
    float weight;
};

IesInterval FindIesInterval(float* angles, uint count, float angle)
{
    IesInterval interval;
    interval.low = 0u;
    interval.high = count - 1u;
    interval.weight = 0.0;
    if (angle <= angles[0])
    {
        interval.high = 0u;
        return interval;
    }
    if (angle >= angles[interval.high])
    {
        interval.low = interval.high;
        return interval;
    }
    while (interval.high - interval.low > 1u)
    {
        uint mid = (interval.low + interval.high) / 2u;
        if (angles[mid] <= angle) interval.low = mid;
        else interval.high = mid;
    }
    float lo = angles[interval.low];
    interval.weight = (angle - lo) / (angles[interval.high] - lo);
    return interval;
}

float LocalLightAngularIntensity(PointLightData light, float3 surfaceToLight)
{
    if (light.iesTangent.w == 0.0)
        return SpotLightConeAttenuation(light.directionInnerCos, light.coneOuterCosType, surfaceToLight);

    // The CPU validates each immutable table before publishing its address.
    float* profile = light.iesProfile;
    uint verticalCount = uint(profile[0]);
    uint horizontalCount = uint(profile[1]);
    uint symmetry = uint(profile[2]);
    float* verticalAngles = profile + 4;
    float3 emission = -surfaceToLight;
    float3 forward = light.directionInnerCos.xyz;
    float vertical = degrees(acos(clamp(dot(forward, emission), -1.0, 1.0)));
    if (vertical < verticalAngles[0] || vertical > verticalAngles[verticalCount - 1u]) return 0.0;
    IesInterval v = FindIesInterval(verticalAngles, verticalCount, vertical);
    float* horizontalAngles = verticalAngles + verticalCount;
    float* candela = horizontalAngles + horizontalCount;
    if (symmetry == 0u) return lerp(candela[v.low], candela[v.high], v.weight);

    float3 c0 = light.iesTangent.xyz;
    float2 projection = float2(dot(c0, emission), dot(cross(c0, forward), emission));
    float horizontal = any(projection != 0.0.xx) ? degrees(atan2(projection.y, projection.x)) : 0.0;
    if (horizontal < 0.0) horizontal += 360.0;
    if (symmetry == 1u)
    {
        horizontal = fmod(horizontal, 180.0);
        horizontal = min(horizontal, 180.0 - horizontal);
    }
    else if (symmetry == 2u) horizontal = min(horizontal, 360.0 - horizontal);
    else if (symmetry == 3u)
    {
        if (horizontal < 90.0) horizontal = 180.0 - horizontal;
        if (horizontal > 270.0) horizontal = 540.0 - horizontal;
    }
    IesInterval h = FindIesInterval(horizontalAngles, horizontalCount, horizontal);
    uint lowRow = h.low * verticalCount;
    uint highRow = h.high * verticalCount;
    return lerp(lerp(candela[lowRow + v.low], candela[lowRow + v.high], v.weight),
                lerp(candela[highRow + v.low], candela[highRow + v.high], v.weight), h.weight);
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
    uint areaLightingMode; // 0: sampled; 1: LTC, when its LUTs are available.
    uint areaLightSampleCount; // 9, 25 (legacy/default), or 64.
    float4 environmentOrientation; // cos(yaw), sin(yaw), sky blur, reserved
    float4 environmentBackground; // scene-linear RGB, solid override
    float4 environmentSunDirection; // local direction toward the solar disk
    float4 environmentSunRadiance; // RGB scene-linear radiance, W enables higher-order aerial scattering
    float4 aerialGeometry; // planet radius, atmosphere height, observer height, kilometres per world unit
    float4 aerialDensity; // Rayleigh, Mie, ozone, solar irradiance
    float4 atmosphereLut[128]; // 16 solar cosine samples by 8 altitude samples
};

float3 RotateEnvironmentDirection(float3 direction, float2 yaw)
{
    if (all(yaw == float2(1.0, 0.0)))
        return direction;
    return float3(yaw.x * direction.x - yaw.y * direction.z, direction.y,
                  yaw.y * direction.x + yaw.x * direction.z);
}

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
