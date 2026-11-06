#include "lighting_structs.slang"
#include "brdf_common.slang"
#include "scene_clip_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> camera;
[[vk::binding(1, 0)]] ConstantBuffer<LightingBuffer> lighting;
[[vk::binding(2, 0)]] SamplerState densitySampler;
[[vk::binding(3, 0)]] Texture3D<float> densityTextures[4];
[[vk::binding(4, 0)]] Texture2D<float> sceneDepth;
[[vk::binding(5, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> sceneColor;
// Binding 6 is SceneClipState from scene_clip_common.slang.
[[vk::binding(7, 0)]] StructuredBuffer<PointLightData> pointLights;
[[vk::binding(8, 0)]] Texture3D<float> temperatureTextures[4];

struct MediumData {
    float4 minimumDensity;
    float4 maximumStep;
    float4 scatteringAnisotropy;
    float4 emission;
    float4 temperatureControls; // Kelvin multiplier, sparse scale, higher-order strength
    uint4 flags;               // x: enabled=1, temperature=2, sparse=4, geometry shadows=16; y: extra orders
};
[[vk::binding(9, 0)]] StructuredBuffer<MediumData> media;
[[vk::binding(14, 0)]] StructuredBuffer<uint> sparseDensity[4];

#include "openvdb_geometry_shadow.slang"
#include "openvdb_sparse.slang"

struct VolumeConstants {
    float4 minimumStep;    // union minimum, smallest target step of enabled media
    float4 maximumPadding;
    uint4 extentFlags;     // width, height, orthographic, count | section-plane flag256
    float4 sectionPlane;
};
[[vk::push_constant]] ConstantBuffer<VolumeConstants> pc;

#include "openvdb_transparency.slang"

uint MediumCount() { return min(pc.extentFlags.w & 255u, 4u); }

bool IntersectVolume(float3 origin, float3 direction, out float start, out float end) {
    start = -1e30;
    end = 1e30;
    [unroll] for (uint axis = 0; axis < 3; ++axis) {
        if (direction[axis] == 0.0) {
            if (origin[axis] < pc.minimumStep[axis] || origin[axis] > pc.maximumPadding[axis]) return false;
        } else {
            float a = (pc.minimumStep[axis] - origin[axis]) / direction[axis];
            float b = (pc.maximumPadding[axis] - origin[axis]) / direction[axis];
            start = max(start, min(a, b));
            end = min(end, max(a, b));
        }
    }
    return end > max(start, 0.0);
}

float MediumEmptyDistance(uint index, float3 world, float3 direction) {
    if (index >= MediumCount() || (media[index].flags.x & 1u) == 0u) return 1e30;
    float entry = -1e30;
    float exit = 1e30;
    [unroll] for (uint axis = 0; axis < 3; ++axis) {
        if (direction[axis] == 0.0) {
            if (world[axis] < media[index].minimumDensity[axis] ||
                world[axis] > media[index].maximumStep[axis]) return 1e30;
        } else {
            float a = (media[index].minimumDensity[axis] - world[axis]) / direction[axis];
            float b = (media[index].maximumStep[axis] - world[axis]) / direction[axis];
            entry = max(entry, min(a, b));
            exit = min(exit, max(a, b));
        }
    }
    if (exit <= max(entry, 0.0)) return 1e30;
    if (entry > 0.0) return entry;
    // Dense media have no topology to prove an interval empty.
    [branch] if ((media[index].flags.x & 4u) == 0u) return 0.0;
    float scale = media[index].temperatureControls.y;
    switch (index) {
    case 0u: return NanoVdbEmptyDistance(sparseDensity[0], world, direction, scale);
    case 1u: return NanoVdbEmptyDistance(sparseDensity[1], world, direction, scale);
    case 2u: return NanoVdbEmptyDistance(sparseDensity[2], world, direction, scale);
    default: return NanoVdbEmptyDistance(sparseDensity[3], world, direction, scale);
    }
}

float3 VolumeUv(uint index, float3 world) {
    return (world - media[index].minimumDensity.xyz) /
           (media[index].maximumStep.xyz - media[index].minimumDensity.xyz);
}

// Literal descriptor indices avoid requiring non-uniform descriptor indexing.
float DenseDensity(uint index, float3 uvw) {
    switch (index) {
    case 0u: return densityTextures[0].SampleLevel(densitySampler, uvw, 0.0);
    case 1u: return densityTextures[1].SampleLevel(densitySampler, uvw, 0.0);
    case 2u: return densityTextures[2].SampleLevel(densitySampler, uvw, 0.0);
    default: return densityTextures[3].SampleLevel(densitySampler, uvw, 0.0);
    }
}
float DenseTemperature(uint index, float3 uvw) {
    switch (index) {
    case 0u: return temperatureTextures[0].SampleLevel(densitySampler, uvw, 0.0);
    case 1u: return temperatureTextures[1].SampleLevel(densitySampler, uvw, 0.0);
    case 2u: return temperatureTextures[2].SampleLevel(densitySampler, uvw, 0.0);
    default: return temperatureTextures[3].SampleLevel(densitySampler, uvw, 0.0);
    }
}
float NativeDensity(uint index, float3 world) {
    float scale = media[index].temperatureControls.y;
    switch (index) {
    case 0u: return SampleNanoVdb(sparseDensity[0], world, scale);
    case 1u: return SampleNanoVdb(sparseDensity[1], world, scale);
    case 2u: return SampleNanoVdb(sparseDensity[2], world, scale);
    default: return SampleNanoVdb(sparseDensity[3], world, scale);
    }
}
float NativeTemperature(uint index, float3 world) {
    float scale = media[index].temperatureControls.y;
    uint offset = media[index].flags.z;
    switch (index) {
    case 0u: return SampleNanoVdb(sparseDensity[0], world, scale, offset);
    case 1u: return SampleNanoVdb(sparseDensity[1], world, scale, offset);
    case 2u: return SampleNanoVdb(sparseDensity[2], world, scale, offset);
    default: return SampleNanoVdb(sparseDensity[3], world, scale, offset);
    }
}

float SampleRawDensity(uint index, float3 world) {
    if (index >= MediumCount() || (media[index].flags.x & 1u) == 0u) return 0.0;
    float3 uvw = VolumeUv(index, world);
    if (any(uvw < 0.0.xxx) || any(uvw > 1.0.xxx) ||
        SceneClipPlanesClip(world, pc.extentFlags.w & 256u, pc.sectionPlane)) return 0.0;
    [branch] if ((media[index].flags.x & 4u) != 0u)
        return max(NativeDensity(index, world), 0.0);
    return max(DenseDensity(index, uvw), 0.0);
}

float SampleExtinction(float3 world) {
    float extinction = 0.0;
    [unroll] for (uint index = 0; index < 4; ++index)
        if (index < MediumCount())
            extinction += SampleRawDensity(index, world) * media[index].minimumDensity.w;
    return extinction;
}

float LightTransmittance(float3 world, float3 toLight, float lightDistance, uint samples) {
    float start, end;
    if (!IntersectVolume(world, toLight, start, end)) return 1.0;
    // A local light is not shadowed by medium beyond the light.
    end = min(max(end, 0.0), lightDistance);
    float step = end / float(samples);
    float opticalDepth = 0.0;
    [loop] for (uint i = 0; i < samples; ++i)
        opticalDepth += SampleExtinction(world + toLight * ((float(i) + 0.5) * step)) * step;
    return exp(-min(opticalDepth, 40.0));
}

float PhaseWeight(float cosine, float g) {
    float denominator = max(1.0 + g * g - 2.0 * g * clamp(cosine, -1.0, 1.0), 0.0025);
    return (1.0 - g * g) / (denominator * sqrt(denominator));
}

float3 ScatteringSource(float3 coefficients[4], float cosine, float geometryVisibility) {
    float3 result = 0.0.xxx;
    [unroll] for (uint index = 0; index < 4; ++index) {
        if (index >= MediumCount()) continue;
        float visibility = (media[index].flags.x & 16u) != 0u ? geometryVisibility : 1.0;
        result += coefficients[index] * PhaseWeight(cosine, media[index].scatteringAnisotropy.w) * visibility;
    }
    return result;
}

#include "openvdb_multiple_scattering.slang"

float3 LocalSource(float3 world, float3 viewDirection, float3 coefficients[4],
                   float3 mixtureAlbedo, bool multipleScattering) {
    float3 result = 0.0.xxx;
    uint count = min(lighting.pointLightCount, 64u);
    [loop] for (uint index = 0; index < count; ++index) {
        PointLightData light = pointLights[index];
        if (!PointLightHasRenderableIntensity(light) || !all(isfinite(light.positionRadius))) continue;
        float3 delta = light.positionRadius.xyz - world;
        float distanceSq = dot(delta, delta);
        if (!isfinite(distanceSq) || distanceSq < 1e-10) continue;
        float distance = sqrt(distanceSq);
        float3 toLight = delta / distance;
        float attenuation = PointLightAttenuation(distanceSq, light.positionRadius.w) *
                            LocalLightAngularIntensity(light, toLight);
        if (attenuation <= 1e-6) continue;
        float visibility = LightTransmittance(world, toLight, distance, 4u);
        float geometryVisibility = VolumeLocalGeometryVisibility(world, index, light);
        result += max(light.colorIntensity.rgb, 0.0.xxx) * light.colorIntensity.a *
                  (0.25 * attenuation * visibility) *
                  ScatteringSource(coefficients, dot(viewDirection, toLight), geometryVisibility);
        if (multipleScattering)
            result += max(light.colorIntensity.rgb, 0.0.xxx) * light.colorIntensity.a *
                      (0.25 * attenuation * visibility) *
                      VolumeMultipleScatteringSource(coefficients, mixtureAlbedo, visibility, geometryVisibility);
    }
    return result;
}

#include "openvdb_area_light.slang"

float3 TemperatureHue(float kelvin) {
    if (kelvin <= 0.0 || !isfinite(kelvin)) return 0.0.xxx;
    // Three Planck-radiance bands, white-balanced at 6500 K. Emission scale sets
    // brightness; this preview hue is not a radiometric calibration.
    const float3 c2OverWavelength = 14387.76877 / float3(0.65, 0.55, 0.45);
    float3 reference = exp(c2OverWavelength / 6500.0) - 1.0.xxx;
    float3 bands = reference / max(exp(min(c2OverWavelength / max(kelvin, 300.0), 80.0.xxx)) - 1.0.xxx, 1e-6.xxx);
    return saturate(bands / max(max(bands.r, bands.g), max(bands.b, 1e-30)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= pc.extentFlags.x || id.y >= pc.extentFlags.y) return;
    float4 background = sceneColor[id.xy];
    // Alpha carries exact visible-medium coverage only while this pass runs.
    sceneColor[id.xy] = float4(background.rgb, 0.0);
    float2 uv = (float2(id.xy) + 0.5.xx) / float2(pc.extentFlags.xy);
    float3 nearWorld, referenceWorld;
    if (!TryReconstructWorldPosition(uv, 1.0, camera.inverseViewProj, nearWorld) ||
        !TryReconstructWorldPosition(uv, 0.5, camera.inverseViewProj, referenceWorld)) return;
    float3 origin = pc.extentFlags.z != 0u ? nearWorld : camera.cameraWorldPosition.xyz;
    float3 direction = SafeNormalize(referenceWorld - origin, float3(0.0, 0.0, -1.0));
    float start, end;
    if (!IntersectVolume(origin, direction, start, end)) return;
    start = max(start, max(dot(nearWorld - origin, direction), 0.0));
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 farHomogeneous = mul(camera.inverseViewProj, float4(ndc, 0.0, 1.0));
    if (all(isfinite(farHomogeneous)) && farHomogeneous.w > 0.0) {
        float3 farWorld = farHomogeneous.xyz / farHomogeneous.w;
        if (all(isfinite(farWorld))) end = min(end, dot(farWorld - origin, direction));
    }
    float depth = sceneDepth.Load(int3(id.xy, 0));
    float3 opaqueWorld;
    if (depth > 0.0 && TryReconstructWorldPosition(uv, depth, camera.inverseViewProj, opaqueWorld))
        end = min(end, dot(opaqueWorld - origin, direction));
    if (end <= start) return;
    VolumeTransparentLayers transparentLayers = LoadVolumeTransparentLayers(id.xy);
    bool native = false;
    [unroll] for (uint mediumIndex = 0; mediumIndex < 4; ++mediumIndex)
        if (mediumIndex < MediumCount() && (media[mediumIndex].flags.x & 5u) == 5u) native = true;
    uint count = uint(clamp(ceil((end - start) / max(pc.minimumStep.w, 1e-6)), 1.0, 768.0));
    // Native grids retain their physical sampling step regardless of world
    // bounds. Empty topology/AABB gaps consume one jump, not hundreds of steps.
    float baseStep = native ? pc.minimumStep.w : (end - start) / float(count);
    uint budget = native ? 4096u : count;
    float transmittance = 1.0;
    float3 integrated = 0.0.xxx;
    bool contributes = false;
    // Glass in front of the volume attenuates all later medium/opaque radiance.
    IntegrateVolumeLayeredInterval(0.0, start, 0.0, 0.0.xxx, uv, origin, direction,
                                  transparentLayers, integrated, transmittance, contributes);
    float3 toLight = -SafeNormalize(lighting.directionalDirection.xyz, float3(0.0, -1.0, 0.0));
    float3 directional = max(lighting.directionalColorIntensity.rgb, 0.0.xxx) *
                         max(lighting.directionalColorIntensity.a, 0.0);
    float3 ambient = (0.05 + 0.2 * max(lighting.environmentIntensity, 0.0)).xxx;
    bool multipleScattering = VolumeMultipleScatteringEnabled();
    float rayDistance = start;
    [loop] for (uint i = 0; i < budget && rayDistance < end && transmittance > 0.005; ++i) {
        if (native) {
            float jump = 1e30;
            [unroll] for (uint mediumIndex = 0; mediumIndex < 4; ++mediumIndex)
                if (mediumIndex < MediumCount())
                    jump = min(jump, MediumEmptyDistance(mediumIndex, origin + direction * rayDistance, direction));
            if (jump > baseStep) {
                float nextDistance = min(end, rayDistance + jump);
                IntegrateVolumeLayeredInterval(rayDistance, nextDistance - rayDistance, 0.0, 0.0.xxx,
                                              uv, origin, direction, transparentLayers, integrated, transmittance, contributes);
                rayDistance = nextDistance;
                continue;
            }
        }
        float step = native ? min(baseStep, end - rayDistance) : baseStep;
        float segmentStart = rayDistance;
        float3 world = origin + direction * (rayDistance + 0.5 * step);
        rayDistance = native ? rayDistance + step : start + float(i + 1u) * baseStep;
        float extinction = 0.0;
        float3 emitted = 0.0.xxx;
        float3 totalScattering = 0.0.xxx;
        float3 coefficients[4];
        [unroll] for (uint mediumIndex = 0; mediumIndex < 4; ++mediumIndex) {
            coefficients[mediumIndex] = 0.0.xxx;
            if (mediumIndex >= MediumCount()) continue;
            MediumData medium = media[mediumIndex];
            float density = SampleRawDensity(mediumIndex, world);
            if (density <= 0.0) continue;
            float sigma = density * medium.minimumDensity.w;
            extinction += sigma;
            coefficients[mediumIndex] = sigma * medium.scatteringAnisotropy.rgb;
            totalScattering += coefficients[mediumIndex];
            float3 emission = medium.emission.rgb * medium.emission.a;
            if ((medium.flags.x & 2u) != 0u && medium.emission.a > 0.0) {
                float temperature;
                [branch] if ((medium.flags.x & 4u) != 0u)
                    temperature = NativeTemperature(mediumIndex, world);
                else temperature = DenseTemperature(mediumIndex, VolumeUv(mediumIndex, world));
                emission *= TemperatureHue(max(temperature, 0.0) * medium.temperatureControls.x);
            }
            emitted += density * emission;
        }
        if (extinction <= 0.0 && !any(emitted > 0.0.xxx)) {
            IntegrateVolumeLayeredInterval(segmentStart, step, 0.0, 0.0.xxx,
                                          uv, origin, direction, transparentLayers, integrated, transmittance, contributes);
            continue;
        }
        float3 source = totalScattering * ambient + emitted;
        if (any(totalScattering > 0.0.xxx)) {
            float3 mixtureAlbedo = multipleScattering ? saturate(totalScattering / extinction) : 0.0.xxx;
            if (any(directional > 0.0.xxx)) {
                float geometryVisibility = VolumeDirectionalGeometryVisibility(world);
                float visibility = LightTransmittance(world, toLight, 1e30, 8u);
                source += directional * (0.25 * visibility) *
                          ScatteringSource(coefficients, dot(direction, toLight), geometryVisibility);
                if (multipleScattering)
                    source += directional * (0.25 * visibility) *
                              VolumeMultipleScatteringSource(coefficients, mixtureAlbedo, visibility, geometryVisibility);
            }
            source += LocalSource(world, direction, coefficients, mixtureAlbedo, multipleScattering);
            source += AreaSource(world, direction, coefficients, mixtureAlbedo, multipleScattering);
        }
        // Sum all extinction and source coefficients BEFORE integration. This
        // makes overlapping media independent of source submission order.
        IntegrateVolumeLayeredInterval(segmentStart, step, extinction, source, uv, origin, direction,
                                      transparentLayers, integrated, transmittance, contributes);
    }
    FinishVolumeTransparentLayers(id.xy, transparentLayers, integrated, transmittance);
    sceneColor[id.xy] = float4(clamp(integrated + transmittance * background.rgb, 0.0.xxx, 65000.0.xxx),
                               contributes || transparentLayers.count > 0u ? 1.0 : 0.0);
}
