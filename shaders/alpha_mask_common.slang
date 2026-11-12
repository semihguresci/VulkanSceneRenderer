#pragma once

float Bayer4x4Threshold(uint2 pixelCoord)
{
    uint index = (pixelCoord.x & 3u) | ((pixelCoord.y & 3u) << 2u);
    uint bayer = 0u;
    switch (index)
    {
    case 0u:  bayer = 0u;  break;
    case 1u:  bayer = 8u;  break;
    case 2u:  bayer = 2u;  break;
    case 3u:  bayer = 10u; break;
    case 4u:  bayer = 12u; break;
    case 5u:  bayer = 4u;  break;
    case 6u:  bayer = 14u; break;
    case 7u:  bayer = 6u;  break;
    case 8u:  bayer = 3u;  break;
    case 9u:  bayer = 11u; break;
    case 10u: bayer = 1u;  break;
    case 11u: bayer = 9u;  break;
    case 12u: bayer = 15u; break;
    case 13u: bayer = 7u;  break;
    case 14u: bayer = 13u; break;
    default:  bayer = 5u;  break;
    }
    return (float(bayer) + 0.5) / 16.0;
}

float UntexturedAlphaMaskDistance(float vertexAlpha, GpuMaterial material)
{
    const float alpha = saturate(material.color.a * vertexAlpha * saturate(material.opacityFactor));
    return ResolveGpuMaterialAlpha(alpha, material.flags) - material.alphaCutoff;
}

bool AlphaMaskUsesTexture(GpuMaterial material)
{
    return material.baseColorTextureIndex != kInvalidGpuMaterialTexture ||
           material.opacityTextureIndex != kInvalidGpuMaterialTexture;
}

float RasterBaseAlpha(float interpolatedAlpha, float flatVertexAlpha, GpuMaterial material)
{
    if ((material.flags & kGpuMaterialFlagAlphaMask) == 0u || !AlphaMaskUsesTexture(material))
        return interpolatedAlpha;

    // Perspective interpolation can add roundoff to a constant vertex alpha.
    // Recover the authored value within that precision before sampling textures.
    const float flatAlpha = material.color.a * flatVertexAlpha;
    const float difference = interpolatedAlpha - flatAlpha;
    const float variation = abs(ddx(difference)) + abs(ddy(difference));
    const float interpolationTolerance = abs(flatAlpha) * (4.0 * 1.1920928955078125e-7);
    return abs(difference) + variation <= interpolationTolerance ? flatAlpha : interpolatedAlpha;
}

bool AlphaMaskPass(float alpha, float untexturedDistance, GpuMaterial material, float4 pixelPosition)
{
    if (material.alphaCutoff <= 0.0) return true;
    if (material.alphaCutoff > 1.0) return false;

    // Interpolate the signed distance for untextured masks. Exact equality
    // stays zero, and perspective rounding cannot erase a nearby cutoff sign.
    const float distanceToCutoff = AlphaMaskUsesTexture(material)
        ? alpha - material.alphaCutoff : untexturedDistance;
    const float alphaWidth = abs(ddx(distanceToCutoff)) + abs(ddy(distanceToCutoff));
    if (alphaWidth == 0.0) return distanceToCutoff >= 0.0;
    float coverage = saturate(distanceToCutoff / alphaWidth + 0.5);
    if (coverage <= 0.0) return false;
    if (coverage >= 1.0) return true;
    return coverage >= Bayer4x4Threshold(uint2(pixelPosition.xy));
}
