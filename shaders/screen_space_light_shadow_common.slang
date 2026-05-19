// screen_space_light_shadow_common.slang - Shared screen-space local light
// contact visibility for point/spot deferred lighting paths. This uses the
// already-rendered opaque scene depth as a contact cue; it is not a local shadow map.
// It does not replace punctual or area shadow-map support.
#pragma once

#include "brdf_common.slang"

static const uint SCREEN_SPACE_LIGHT_SHADOW_STEPS = 24u;
static const float SCREEN_SPACE_LIGHT_SHADOW_MIN_RAY_RADIUS = 0.075;
static const uint SCREEN_SPACE_DIRECTIONAL_CONTACT_SHADOW_STEPS = 16u;
static const float SCREEN_SPACE_DIRECTIONAL_CONTACT_MIN_THICKNESS = 0.015;

float2 ProjectWorldToSceneUv(float3 worldPosition,
                             column_major float4x4 viewProj,
                             out bool valid)
{
    if (!all(isfinite(worldPosition)))
    {
        valid = false;
        return 0.0.xx;
    }

    float4 clip = mul(viewProj, float4(worldPosition, 1.0));
    if (!all(isfinite(clip)) || clip.w <= 1e-6)
    {
        valid = false;
        return 0.0.xx;
    }

    float3 ndc = clip.xyz / clip.w;
    valid = all(isfinite(ndc)) &&
            all(ndc.xy >= -1.0.xx) && all(ndc.xy <= 1.0.xx) &&
            ndc.z >= 0.0 && ndc.z <= 1.0;
    return float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
}

bool TryProjectWorldToScenePixel(float3 worldPosition,
                                 uint width,
                                 uint height,
                                 column_major float4x4 viewProj,
                                 out uint2 pixel)
{
    pixel = uint2(0u, 0u);
    if (width == 0u || height == 0u)
    {
        return false;
    }

    bool sampleValid = false;
    float2 sampleUv = ProjectWorldToSceneUv(worldPosition, viewProj,
                                            sampleValid);
    if (!sampleValid || !all(isfinite(sampleUv)))
    {
        return false;
    }

    float2 extent = float2(width, height);
    float2 samplePixel = sampleUv * extent;
    if (!all(isfinite(samplePixel)) ||
        any(samplePixel < 0.0.xx) ||
        samplePixel.x >= extent.x ||
        samplePixel.y >= extent.y)
    {
        return false;
    }

    pixel = uint2(samplePixel);
    return true;
}

float ScreenSpaceDirectionalContactRayJitter(uint2 pixel)
{
    float pixelSeed = dot(float2(pixel), float2(0.06711056, 0.00583715));
    return frac(52.9829189 * frac(pixelSeed));
}

float ScreenSpacePointLightContactVisibility(
    float3 worldPosition,
    float3 receiverNormal,
    float3 lightPosition,
    Texture2D<float> depthTexture,
    Texture2D<float4> normalTexture,
    column_major float4x4 viewProj,
    column_major float4x4 inverseViewProj)
{
    uint width = 0u;
    uint height = 0u;
    depthTexture.GetDimensions(width, height);
    if (!all(isfinite(worldPosition)) || !all(isfinite(lightPosition)))
    {
        return 1.0;
    }

    if (width == 0u || height == 0u)
    {
        return 1.0;
    }

    float3 lightVector = lightPosition - worldPosition;
    float lightDistance = length(lightVector);
    if (!isfinite(lightDistance) || lightDistance <= 1e-4)
    {
        return 1.0;
    }

    float3 lightDir = lightVector / lightDistance;
    float3 receiverN = SafeNormalize(receiverNormal, -lightDir);
    float surfaceBias = max(lightDistance * 0.015, 0.03);
    float baseRayRadius =
        max(lightDistance * 0.018, SCREEN_SPACE_LIGHT_SHADOW_MIN_RAY_RADIUS);
    float2 extent = float2(width, height);
    float visibility = 1.0;

    [loop]
    for (uint sampleIndex = 1u;
         sampleIndex <= SCREEN_SPACE_LIGHT_SHADOW_STEPS; ++sampleIndex)
    {
        float t = (float(sampleIndex) + 0.35) /
                  (float(SCREEN_SPACE_LIGHT_SHADOW_STEPS) + 1.35);
        float sampleDistance = t * lightDistance;
        if (sampleDistance <= surfaceBias ||
            sampleDistance >= lightDistance - surfaceBias)
        {
            continue;
        }

        float3 samplePosition = worldPosition + lightDir * sampleDistance;
        uint2 samplePixel = uint2(0u, 0u);
        if (!TryProjectWorldToScenePixel(samplePosition, width, height,
                                         viewProj, samplePixel))
        {
            continue;
        }

        float sceneDepth = depthTexture.Load(int3(samplePixel, 0));
        if (!isfinite(sceneDepth) || sceneDepth <= 0.0 || sceneDepth > 1.0)
        {
            continue;
        }

        float2 sceneUv = (float2(samplePixel) + 0.5.xx) / extent;
        float3 sceneWorld;
        if (!TryReconstructWorldPosition(sceneUv, sceneDepth,
                                         inverseViewProj, sceneWorld))
        {
            continue;
        }

        float3 sceneNormal = SafeNormalize(
            normalTexture.Load(int3(samplePixel, 0)).xyz * 2.0 - 1.0,
            receiverN);
        float receiverPlaneDistance =
            abs(dot(sceneWorld - worldPosition, receiverN));
        bool coplanarReceiverSurface =
            dot(sceneNormal, receiverN) > 0.9 &&
            receiverPlaneDistance <= max(0.025, lightDistance * 0.012);
        if (coplanarReceiverSurface)
        {
            continue;
        }

        float3 receiverToScene = sceneWorld - worldPosition;
        float sceneDistanceAlongRay = dot(receiverToScene, lightDir);
        if (sceneDistanceAlongRay <= surfaceBias ||
            sceneDistanceAlongRay >= lightDistance - surfaceBias)
        {
            continue;
        }

        float3 closestRayPoint = lightDir * sceneDistanceAlongRay;
        float distanceFromLightRay =
            length(receiverToScene - closestRayPoint);
        float rayRadius = baseRayRadius + sceneDistanceAlongRay * 0.004;
        if (distanceFromLightRay <= rayRadius)
        {
            visibility = 0.0;
            break;
        }
    }

    return saturate(visibility);
}

float ScreenSpaceDirectionalContactVisibility(
    float3 worldPosition,
    float3 receiverNormal,
    float3 surfaceToLight,
    float maxRayDistance,
    float thicknessWorld,
    float fadeDistanceWorld,
    Texture2D<float> depthTexture,
    Texture2D<float4> normalTexture,
    column_major float4x4 viewProj,
    column_major float4x4 inverseViewProj)
{
    uint width = 0u;
    uint height = 0u;
    depthTexture.GetDimensions(width, height);
    if (width == 0u || height == 0u ||
        !all(isfinite(worldPosition)) ||
        !all(isfinite(receiverNormal)) ||
        !all(isfinite(surfaceToLight)) ||
        !isfinite(maxRayDistance) || maxRayDistance <= 1.0e-4)
    {
        return 1.0;
    }

    float lightLengthSq = dot(surfaceToLight, surfaceToLight);
    if (!isfinite(lightLengthSq) || lightLengthSq <= 1.0e-8)
    {
        return 1.0;
    }

    float3 lightDir = surfaceToLight * rsqrt(lightLengthSq);
    float3 receiverN = SafeNormalize(receiverNormal, -lightDir);
    float grazingNormalFade =
        saturate((dot(receiverN, lightDir) - 0.05) / 0.25);
    if (grazingNormalFade <= 0.0)
    {
        return 1.0;
    }

    float maxDistance = max(maxRayDistance, 1.0e-4);
    float rayThickness = max(thicknessWorld,
                             SCREEN_SPACE_DIRECTIONAL_CONTACT_MIN_THICKNESS);
    float fadeDistance = max(fadeDistanceWorld, rayThickness);
    float surfaceBias = max(rayThickness * 1.5, maxDistance * 0.01);
    float2 extent = float2(width, height);
    float visibility = 1.0;
    uint2 receiverPixel = uint2(0u, 0u);
    TryProjectWorldToScenePixel(worldPosition, width, height, viewProj,
                                receiverPixel);
    float rayJitter =
        (ScreenSpaceDirectionalContactRayJitter(receiverPixel) - 0.5) * 0.25;

    [loop]
    for (uint sampleIndex = 0u;
         sampleIndex < SCREEN_SPACE_DIRECTIONAL_CONTACT_SHADOW_STEPS;
         ++sampleIndex)
    {
        float sampleT =
            clamp((float(sampleIndex) + 0.5 + rayJitter) /
                      float(SCREEN_SPACE_DIRECTIONAL_CONTACT_SHADOW_STEPS),
                  1.0e-4,
                  1.0);
        float sampleDistance =
            lerp(surfaceBias, maxDistance, sampleT * sampleT);
        float3 samplePosition = worldPosition + lightDir * sampleDistance;

        uint2 samplePixel = uint2(0u, 0u);
        if (!TryProjectWorldToScenePixel(samplePosition, width, height,
                                         viewProj, samplePixel))
        {
            continue;
        }

        float sceneDepth = depthTexture.Load(int3(samplePixel, 0));
        if (!isfinite(sceneDepth) || sceneDepth <= 0.0 || sceneDepth > 1.0)
        {
            continue;
        }

        float2 sceneUv = (float2(samplePixel) + 0.5.xx) / extent;
        float3 sceneWorld;
        if (!TryReconstructWorldPosition(sceneUv, sceneDepth,
                                         inverseViewProj, sceneWorld))
        {
            continue;
        }

        float3 sceneNormal = SafeNormalize(
            normalTexture.Load(int3(samplePixel, 0)).xyz * 2.0 - 1.0,
            receiverN);
        float receiverPlaneDistance =
            abs(dot(sceneWorld - worldPosition, receiverN));
        bool coplanarReceiverSurface =
            dot(sceneNormal, receiverN) > 0.9 &&
            receiverPlaneDistance <= max(rayThickness * 1.5,
                                         sampleDistance * 0.025);
        if (coplanarReceiverSurface)
        {
            continue;
        }

        float3 receiverToScene = sceneWorld - worldPosition;
        float sceneDistanceAlongRay = dot(receiverToScene, lightDir);
        if (sceneDistanceAlongRay <= surfaceBias ||
            sceneDistanceAlongRay > sampleDistance + rayThickness ||
            sceneDistanceAlongRay >= maxDistance)
        {
            continue;
        }

        float3 closestRayPoint = lightDir * sceneDistanceAlongRay;
        float distanceFromLightRay =
            length(receiverToScene - closestRayPoint);
        float adaptiveRayThickness =
            rayThickness + sceneDistanceAlongRay * 0.02;
        if (distanceFromLightRay <= adaptiveRayThickness)
        {
            visibility = min(
                visibility,
                saturate(sceneDistanceAlongRay /
                         max(fadeDistance, rayThickness)));
            if (visibility <= 0.02)
            {
                break;
            }
        }
    }

    return lerp(1.0, saturate(visibility), grazingNormalFade);
}
