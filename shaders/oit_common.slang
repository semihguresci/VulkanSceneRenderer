#pragma once

static const uint INVALID_NODE_INDEX = 0xffffffffu;
static const uint MAX_OIT_NODES_PER_PIXEL = 64u;
static const uint OIT_COUNTER_STRIPE_COUNT = 64u;
static const uint OIT_COUNTER_OVERFLOW_INDEX = OIT_COUNTER_STRIPE_COUNT;
static const uint OIT_NODE_DISPLAY_OVERLAY = 1u;
// Bit zero filters display overlays in diagnostic views. The next three bits
// order cap fill, hatch, ordinary overlays and selection from back to front.
static const uint OIT_NODE_OVERLAY_PRIORITY_MASK = 0xeu;
static const uint OIT_NODE_STANDARD_OVERLAY = OIT_NODE_DISPLAY_OVERLAY | (3u << 1u);

struct TransparentNode
{
    float4 color;
    float depth;
    uint next;
    uint flags;
    uint padding;
};

bool OitNodePrecedes(TransparentNode a, TransparentNode b)
{
    float aDepth = isfinite(a.depth) ? a.depth : 0.0;
    float bDepth = isfinite(b.depth) ? b.depth : 0.0;
    if (aDepth != bDepth)
        return aDepth > bDepth;
    // Preserve the overlay layer order at equal reverse-Z depth.
    return (a.flags & OIT_NODE_OVERLAY_PRIORITY_MASK) >
           (b.flags & OIT_NODE_OVERLAY_PRIORITY_MASK);
}

struct OitMetadataBuffer
{
    uint nodeCapacity;
    uint viewportWidth;
    uint viewportHeight;
    uint counterLimit;
};
