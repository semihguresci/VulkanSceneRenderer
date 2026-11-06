#include "lighting_structs.slang"

// Set zero is the ACTUAL live tile-cull set 2. Set one independently names
// the current manager scratch allocations so stale production descriptors
// cannot pass by merely writing/readback through their own old bindings.
[[vk::binding(0, 0)]] RWStructuredBuffer<TileLightGrid> uPublishedGrid;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> uPublishedIndices;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> uPublishedStats;
[[vk::binding(0, 1)]] RWStructuredBuffer<uint4> uResult;
[[vk::binding(1, 1)]] RWStructuredBuffer<TileLightGrid> uCurrentGrid;
[[vk::binding(2, 1)]] RWStructuredBuffer<uint> uCurrentIndices;
[[vk::binding(3, 1)]] RWStructuredBuffer<uint> uCurrentStats;
struct Query { uint mode; };
[[vk::push_constant]] ConstantBuffer<Query> query;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID)
{
    if (query.mode == 0)
    {
        TileLightGrid row;
        row.offset = 0x12345678;
        row.count = 0x23456789;
        uPublishedGrid[0] = row;
        uPublishedIndices[0] = 0x3456789a;
        uPublishedStats[0] = 0x456789ab;
    }
    else if (query.mode == 1)
    {
        TileLightGrid row = uCurrentGrid[0];
        uResult[0] = uint4(row.offset, row.count, uCurrentIndices[0], uCurrentStats[0]);
    }
    else
    {
        TileLightGrid row;
        row.offset = row.count = 0xdeadbeef;
        uCurrentGrid[0] = row;
        uCurrentIndices[0] = 0xdeadbeef;
        uCurrentStats[0] = 0xdeadbeef;
        uResult[0] = uint4(0xdeadbeef);
    }
}
