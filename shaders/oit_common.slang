#pragma once

static const uint INVALID_NODE_INDEX = 0xffffffffu;
static const uint MAX_OIT_NODES_PER_PIXEL = 64u;
static const uint OIT_COUNTER_STRIPE_COUNT = 64u;
static const uint OIT_COUNTER_OVERFLOW_INDEX = OIT_COUNTER_STRIPE_COUNT;
static const uint OIT_NODE_DISPLAY_OVERLAY = 1u;
static const uint OIT_NODE_SELECTION_OVERLAY = 2u;
static const uint OIT_NODE_OVERLAY_PRIORITY = OIT_NODE_DISPLAY_OVERLAY | OIT_NODE_SELECTION_OVERLAY;

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
    // At equal reverse-Z depth, keep highlights above the underlying surface
    // and selection above hover, independent of fragment append order.
    return (a.flags & OIT_NODE_OVERLAY_PRIORITY) >
           (b.flags & OIT_NODE_OVERLAY_PRIORITY);
}

struct OitMetadataBuffer
{
    uint nodeCapacity;
    uint viewportWidth;
    uint viewportHeight;
    uint counterLimit;
};
