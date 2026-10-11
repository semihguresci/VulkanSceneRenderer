#pragma once
#include "material_data_common.slang"
#include "ddgi_lookup_common.slang"

static const uint kDdgiMaterialFlagUnlit = 0x10u;

// The probe trace represents ordinary opaque geometry. A displaced raster
// surface, thin/transmitting surface or transparent/unlit object cannot share
// that diffuse transport contract and keeps its existing lighting fallback.
// Receiver eligibility rejects declared transmission, including texture-backed
// transmission. This aligns both raster paths even though deferred receivers
// currently retain an unsampled transmission factor. Secondary hit shading may
// still treat an actually black transmission texel as an opaque reflector.
DdgiLookupResult LookupDdgiDiffuseReceiver(GpuMaterial material, uint objectFlags,
    float transmissionFactor, float3 world, float3 geometricNormal,
    float3 shadingNormal, float3 viewDirection)
{
    uint flags = material.flags | objectFlags;
    if ((flags & (kGpuMaterialFlagAlphaBlend | kDdgiMaterialFlagUnlit)) != 0u ||
        IsThinSurfaceGpuMaterial(flags) ||
        !isfinite(transmissionFactor) || transmissionFactor > 0.0 ||
        !isfinite(material.transmissionFactor) || material.transmissionFactor > 0.0 ||
        material.transmissionTextureIndex != kInvalidGpuMaterialTexture ||
        !isfinite(material.heightScale) ||
        (material.heightTextureIndex != 0xffffffffu && abs(material.heightScale) > 1e-7))
        return (DdgiLookupResult)0;
    return LookupDdgiIrradiance(world, geometricNormal, shadingNormal, viewDirection);
}

// DDGI stores physical incident irradiance E. Apply the Lambertian 1/PI once
// before the caller applies receiver albedo and diffuse Fresnel/metalness.
// The existing environment fallback keeps its historical brightness convention;
// switching to physically normalized DDGI can therefore change its brightness.
float3 DdgiDiffuseIrradiance(DdgiLookupResult lookup)
{
#if defined(DDGI_DISABLED) && DDGI_DISABLED
    return 0.0.xxx;
#else
    return max(lookup.irradiance, 0.0.xxx) *
        (max(uDdgi.intensityBias.x, 0.0) / PI);
#endif
}
