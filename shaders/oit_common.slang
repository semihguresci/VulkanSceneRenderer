#pragma once

static const uint INVALID_NODE_INDEX = 0xffffffffu;
static const uint MAX_OIT_NODES_PER_PIXEL = 64u;
static const uint OIT_COUNTER_STRIPE_COUNT = 64u;
static const uint OIT_COUNTER_OVERFLOW_INDEX = OIT_COUNTER_STRIPE_COUNT;

struct TransparentNode
{
    float4 color;
    float depth;
    uint next;
    float padding0;
    float padding1;
};

struct OitMetadataBuffer
{
    uint nodeCapacity;
    uint viewportWidth;
    uint viewportHeight;
    uint counterLimit;
};
