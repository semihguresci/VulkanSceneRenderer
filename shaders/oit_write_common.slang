#pragma once
#include "oit_common.slang"

[[vk::binding(0, OIT_WRITE_SET)]] RWTexture2D<uint> headPointerImage;
[[vk::binding(1, OIT_WRITE_SET)]] RWStructuredBuffer<TransparentNode> transparentNodes;
[[vk::binding(2, OIT_WRITE_SET)]] RWStructuredBuffer<uint> nodeCounter;
[[vk::binding(3, OIT_WRITE_SET)]] ConstantBuffer<OitMetadataBuffer> uOit;

uint AllocateOitNode(uint2 pixel)
{
    if (pixel.x >= uOit.viewportWidth || pixel.y >= uOit.viewportHeight ||
        uOit.nodeCapacity == 0u || IsHelperLane())
        return INVALID_NODE_INDEX;

    // Interleave neighboring pixels across independent counters. Saturation
    // preserves overflow demand without ever reusing an index in this frame.
    uint stripe = (pixel.x + 23u * pixel.y) % min(OIT_COUNTER_STRIPE_COUNT, uOit.nodeCapacity);
    uint stripeLimit = (uOit.counterLimit - 2u - stripe) / OIT_COUNTER_STRIPE_COUNT + 2u;
    uint ordinal;
    InterlockedAdd(nodeCounter[stripe], 0u, ordinal);
    [loop]
    while (ordinal < stripeLimit)
    {
        uint previousCount;
        InterlockedCompareExchange(nodeCounter[stripe], ordinal, ordinal + 1u, previousCount);
        if (previousCount == ordinal)
            break;
        ordinal = previousCount;
    }
    uint nodeIndex = ordinal * OIT_COUNTER_STRIPE_COUNT + stripe;
    if (nodeIndex < uOit.nodeCapacity)
        return nodeIndex;

    // One fragment per stripe is sufficient to flag any dropped nodes.
    uint stripeCapacity = (uOit.nodeCapacity - 1u - stripe) / OIT_COUNTER_STRIPE_COUNT + 1u;
    if (ordinal == stripeCapacity)
        InterlockedOr(nodeCounter[OIT_COUNTER_OVERFLOW_INDEX], 1u);
    return INVALID_NODE_INDEX;
}

void AppendOitNode(uint2 pixel, uint nodeIndex, float depth, float4 color,
                   uint flags = 0u)
{
    TransparentNode node;
    node.color = float4(color.rgb, saturate(color.a));
    node.depth = saturate(depth);
    node.flags = flags;
    node.padding = 0u;
    uint previousHead = INVALID_NODE_INDEX;
    InterlockedExchange(headPointerImage[pixel], nodeIndex, previousHead);
    node.next = previousHead;
    transparentNodes[nodeIndex] = node;
}
