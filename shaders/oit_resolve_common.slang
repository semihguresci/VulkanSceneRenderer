#pragma once
float4 CompositeFrontToBack(float3 sceneColor, uint sortedIndices[MAX_OIT_NODES_PER_PIXEL], uint nodeCount,
                            out float3 transparentColor, out float revealage)
{
    // Transparent nodes are already sorted nearest-to-farthest by the caller.
    // Front-to-back compositing lets revealage track how much opaque scene
    // color remains visible through the accumulated alpha.
    float3 safeSceneColor = FiniteColorOr(sceneColor, 0.0.xxx);
    float3 accumColor = 0.0.xxx;
    float accumAlpha = 0.0;

    for (uint i = 0u; i < nodeCount; ++i)
    {
        TransparentNode node = transparentNodes[sortedIndices[i]];
        float alpha = saturate(FiniteOr(node.color.a, 0.0));
        float weight =
            saturate(FiniteOr((1.0 - accumAlpha) * alpha, 0.0));
        accumColor += FiniteColorOr(node.color.rgb, 0.0.xxx) * weight;
        accumAlpha = saturate(FiniteOr(accumAlpha + weight, accumAlpha));
    }

    transparentColor = FiniteColorOr(accumColor, 0.0.xxx);
    revealage = saturate(FiniteOr(1.0 - accumAlpha, 1.0));
    return float4(FiniteColorOr(accumColor + safeSceneColor * revealage,
                                safeSceneColor),
                  1.0);
}

struct OitResolveResult
{
    float3 compositedColor;
    float3 transparentColor;
    float revealage;
};

OitResolveResult ResolveOitPixel(uint2 pixelCoord, float3 sceneColor)
{
    OitResolveResult result;
    result.compositedColor = FiniteColorOr(sceneColor, 0.0.xxx);
    result.transparentColor = 0.0.xxx;
    result.revealage = 1.0;

    if (pc.oitEnabled == 0u ||
        pixelCoord.x >= uOit.viewportWidth ||
        pixelCoord.y >= uOit.viewportHeight)
    {
        return result;
    }

    uint current = headPointerImage.Load(int2(pixelCoord));
    if (current == INVALID_NODE_INDEX)
    {
        return result;
    }

    uint sortedIndices[MAX_OIT_NODES_PER_PIXEL];
    uint nodeCount = 0u;
    while (current != INVALID_NODE_INDEX && nodeCount < MAX_OIT_NODES_PER_PIXEL)
    {
        if (current >= uOit.nodeCapacity)
        {
            break;
        }

        TransparentNode node = transparentNodes[current];
        float nodeDepth = FiniteOr(node.depth, 0.0);
        uint insertAt = nodeCount;
        while (insertAt > 0u)
        {
            TransparentNode prevNode = transparentNodes[sortedIndices[insertAt - 1u]];
            if (FiniteOr(prevNode.depth, 0.0) >= nodeDepth)
            {
                break;
            }
            sortedIndices[insertAt] = sortedIndices[insertAt - 1u];
            --insertAt;
        }
        sortedIndices[insertAt] = current;
        ++nodeCount;
        current = node.next;
    }

    if (nodeCount > 0u)
    {
        float4 composited = CompositeFrontToBack(
            sceneColor, sortedIndices, nodeCount,
            result.transparentColor, result.revealage);
        result.compositedColor = composited.rgb;
    }
    return result;
}

