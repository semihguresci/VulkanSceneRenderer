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

bool AlphaMaskPass(float alpha, float cutoff, float4 pixelPosition)
{
    // Flat alpha follows the binary glTF threshold. Filter only varying alpha
    // so equal-to-cutoff interiors have the same coverage in every pass.
    const float alphaWidth = abs(ddx(alpha)) + abs(ddy(alpha));
    if (alphaWidth == 0.0) return alpha >= cutoff;
    float coverage = saturate((alpha - cutoff) / max(alphaWidth, 1e-4) + 0.5);
    if (coverage <= 0.0) return false;
    if (coverage >= 1.0) return true;
    return coverage >= Bayer4x4Threshold(uint2(pixelPosition.xy));
}
