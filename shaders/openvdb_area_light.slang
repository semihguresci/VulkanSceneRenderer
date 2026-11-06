// Bounded area-emitter single scattering. Included after the volume's medium
// transmittance and phase helpers; shared scene helpers define radiance/units.
#pragma once

[[vk::binding(15, 0)]] StructuredBuffer<AreaLightData> volumeAreaLights;

static const uint VOLUME_MAX_AREA_LIGHTS = 8u;
static const uint VOLUME_AREA_LIGHT_SAMPLES = 4u;

float3 VolumeAreaSamplePosition(AreaLightFrame frame, uint sample)
{
    float2 offset;
    if (abs(frame.type - AREA_LIGHT_TYPE_DISK) < 0.5)
    {
        // Centroids of four equal-area disk sectors, matching the four-domain
        // raster shadow origins. Each sample represents one quarter of dA.
        static const float2 signs[4] = {
            float2(1, 1), float2(-1, 1), float2(-1, -1), float2(1, -1)};
        offset = signs[sample] * 0.424413181578;
    }
    else
        offset = float2((sample & 1u) != 0u ? 0.5 : -0.5,
                         (sample & 2u) != 0u ? 0.5 : -0.5);
    return frame.position + frame.tangent * (offset.x * frame.halfSize.x) +
           frame.bitangent * (offset.y * frame.halfSize.y);
}

float VolumeAreaCubeVisibility(float3 world, uint areaIndex, uint cubeBase)
{
    uint maximumLayers = min(volumeLocalShadow.counts.x, MAX_SHADOWED_LOCAL_LIGHT_LAYERS);
    if (cubeBase >= maximumLayers || LOCAL_SHADOW_POINT_FACE_COUNT > maximumLayers - cubeBase)
        return 1.0;
    float3 origin = volumeLocalShadow.layers[cubeBase].positionRange.xyz;
    uint layerIndex = cubeBase + SelectPointLocalShadowFace(world - origin);
    LocalShadowLayerData layer = volumeLocalShadow.layers[layerIndex];
    if (layer.meta.w == 0u || layer.meta.x != areaIndex ||
        layer.directionType.w != LOCAL_SHADOW_TYPE_AREA) return 1.0;
    float distance = length(world - origin);
    float range = layer.positionRange.w;
    if (!isfinite(distance) || !isfinite(range) || range <= 1e-4 ||
        distance <= 1e-4 || distance >= range) return 1.0;
    float4 clip = mul(layer.viewProj, float4(world, 1.0));
    if (!all(isfinite(clip)) || clip.w <= 1e-6) return 1.0;
    float3 projected = clip.xyz / clip.w;
    if (!all(isfinite(projected)) || projected.z < 0.0 || projected.z > 1.0) return 1.0;
    // No surface normal exists at a scattering sample. Match the point/spot
    // medium path's constant bias and raw four-tap reverse-Z PCF.
    float bias = max(LocalShadowFiniteOr(volumeLocalShadow.filterSettings.z, 0.0), 0.0);
    float visibility = VolumeShadowBilinearPcf(volumeLocalShadowAtlas,
        projected.xy * 0.5 + 0.5, layerIndex, projected.z + bias);
    float fade = saturate((range - distance) / max(range * 0.05, 1e-4));
    return lerp(1.0, visibility, fade);
}

float VolumeAreaGeometryVisibility(float3 world, uint areaIndex,
                                    AreaLightData light, AreaLightFrame frame)
{
    if (!LocalShadowMapsEnabled(lighting, volumeLocalShadow)) return 1.0;
    uint encodedLayer = PackedLocalShadowAreaRef(volumeLocalShadow, areaIndex);
    uint sampleCount = LocalAreaShadowSampleCount(encodedLayer, volumeLocalShadow);
    if (sampleCount == 0u) return 1.0;
    float weightedVisibility = 0.0;
    float totalWeight = 0.0;
    // The surface renderer's atlas budget supplies one to four cube origins.
    // Average their geometry visibility independently of the four radiance
    // samples. This is a bounded soft-shadow approximation, not LTC/PCSS.
    [loop] for (uint sample = 0u; sample < sampleCount; ++sample)
    {
        float3 position = LocalAreaShadowSamplePosition(encodedLayer, sample, volumeLocalShadow);
        float3 toLight;
        float weight = AreaLightPositionContributionWeight(light, frame, world, position,
            1.0 / float(sampleCount), toLight);
        if (!isfinite(weight) || weight <= 0.0) continue;
        uint cubeBase = encodedLayer - 1u + sample * LOCAL_SHADOW_POINT_FACE_COUNT;
        weightedVisibility += weight * VolumeAreaCubeVisibility(world, areaIndex, cubeBase);
        totalWeight += weight;
    }
    return totalWeight > 0.0 ? saturate(weightedVisibility / totalWeight) : 1.0;
}

float3 AreaSource(float3 world, float3 viewDirection, float3 coefficients[4],
                 float3 mixtureAlbedo, bool multipleScattering)
{
    float3 result = 0.0.xxx;
    uint count = min(lighting.areaLightCount, VOLUME_MAX_AREA_LIGHTS);
    [loop] for (uint index = 0u; index < count; ++index)
    {
        AreaLightData light = volumeAreaLights[index];
        if (!AreaLightHasRenderableRadiance(light) || !all(isfinite(light.positionRange)) ||
            !all(isfinite(light.directionType)) || !all(isfinite(light.tangentHalfSize)) ||
            !all(isfinite(light.bitangentHalfSize))) continue;
        AreaLightFrame frame = BuildAreaLightFrame(light);
        if (!isfinite(frame.area) || frame.area <= 0.0) continue;
        float3 radiance = max(light.colorIntensity.rgb, 0.0.xxx) * max(light.colorIntensity.a, 0.0);
        float weights[4];
        float distances[4];
        float3 directions[4];
        float solidAngle = 0.0;
        [unroll] for (uint sample = 0u; sample < VOLUME_AREA_LIGHT_SAMPLES; ++sample)
        {
            weights[sample] = 0.0;
            distances[sample] = 0.0;
            directions[sample] = frame.normal;
            float3 position = VolumeAreaSamplePosition(frame, sample);
            float3 toLight;
            // dOmega = cos(theta_emitter) * dA / distance^2. There is no
            // receiving-surface cosine in a participating medium.
            float weight = AreaLightPositionContributionWeight(light, frame, world, position,
                1.0 / float(VOLUME_AREA_LIGHT_SAMPLES), toLight);
            float distance = length(position - world);
            if (!isfinite(weight) || weight <= 0.0 || !isfinite(distance) || distance <= 1e-5) continue;
            weights[sample] = weight;
            distances[sample] = distance;
            directions[sample] = toLight;
            solidAngle += weight;
        }
        if (!isfinite(solidAngle) || solidAngle <= 0.0) continue;
        // A planar one-sided emitter subtends at most a hemisphere. Four-point
        // quadrature can overshoot near an emitter sample; preserve the far
        // field and bound that otherwise singular near-field estimate.
        float angularScale = min(1.0, (2.0 * PI) / solidAngle);
        float geometryVisibility = VolumeAreaGeometryVisibility(world, index, light, frame);
        float phaseMass[4] = {0.0, 0.0, 0.0, 0.0};
        float3 higherOrderMass[4] = {0.0.xxx, 0.0.xxx, 0.0.xxx, 0.0.xxx};
        [unroll] for (uint sample = 0u; sample < VOLUME_AREA_LIGHT_SAMPLES; ++sample)
        {
            if (weights[sample] <= 0.0) continue;
            float3 toLight = directions[sample];
            float visibility = LightTransmittance(world, toLight, distances[sample], 4u);
            // PhaseWeight is HG relative to isotropic; divide by 4pi to
            // integrate the scene's emitted radiance over emitter solid angle.
            float weight = weights[sample] * angularScale * visibility / (4.0 * PI);
            [unroll] for (uint medium = 0u; medium < 4u; ++medium)
                if (medium < MediumCount()) {
                    phaseMass[medium] += weight * PhaseWeight(dot(viewDirection, toLight),
                                                              media[medium].scatteringAnisotropy.w);
                    if (multipleScattering)
                        higherOrderMass[medium] += weight *
                            VolumeMultipleScatteringGain(medium, mixtureAlbedo, visibility);
                }
        }
        [unroll] for (uint medium = 0u; medium < 4u; ++medium)
        {
            if (medium >= MediumCount()) continue;
            float visibility = (media[medium].flags.x & 16u) != 0u ? geometryVisibility : 1.0;
            // A normalized phase distribution integrates to one over the full
            // sphere. Cap a narrow HG lobe that the four samples cannot resolve.
            result += radiance * coefficients[medium] * saturate(phaseMass[medium]) * visibility;
            if (multipleScattering) {
                // Keep the existing unresolved-lobe cap. Additional orders may
                // fill unused phase mass but never reduce the direct result.
                float3 extra = min(higherOrderMass[medium], (1.0 - saturate(phaseMass[medium])).xxx);
                result += radiance * coefficients[medium] * max(extra, 0.0.xxx) * visibility;
            }
        }
    }
    return result;
}
