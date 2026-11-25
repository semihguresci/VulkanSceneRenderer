#include "draw_indirect_common.slang"
#include "bim_residency_flags_common.slang"

struct BimMeshletResidencyEntry
{
    uint flags;
    uint selectedLod;
    uint firstCluster;
    uint clusterCount;
};

struct BimDrawCompactionPushConstants
{
    uint inputDrawCount;
    uint outputCapacity;
    uint objectCount;
    uint flags;
};

[[vk::push_constant]]
ConstantBuffer<BimDrawCompactionPushConstants> pc;

[[vk::binding(0, 0)]] StructuredBuffer<DrawIndexedIndirectCommand> uInputDraws;
[[vk::binding(1, 0)]] StructuredBuffer<BimMeshletResidencyEntry> uResidency;
[[vk::binding(2, 0)]] RWStructuredBuffer<DrawIndexedIndirectCommand> uOutputDraws;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> uOutputCount;
[[vk::binding(4, 0)]] StructuredBuffer<uint> uVisibilityMask;

bool objectIsResident(uint objectIndex)
{
    if (objectIndex >= pc.objectCount)
    {
        return false;
    }
    // Viewport sources refer to published geometry. A mesh without cluster
    // metadata still has a canonical draw and must pass the spatial test.
    const uint requiredFlags = pc.flags == 0u ? BIM_MESHLET_RESIDENT : pc.flags;
    return (uResidency[objectIndex].flags & requiredFlags) == requiredFlags &&
           uVisibilityMask[objectIndex] != 0u;
}

void appendCompactedDraw(DrawIndexedIndirectCommand source, uint objectIndex)
{
    uint outputIndex;
    InterlockedAdd(uOutputCount[0], 1, outputIndex);
    if (outputIndex >= pc.outputCapacity)
    {
        return;
    }

    DrawIndexedIndirectCommand compacted;
    compacted.indexCount = source.indexCount;
    compacted.instanceCount = 1u;
    compacted.firstIndex = source.firstIndex;
    compacted.vertexOffset = source.vertexOffset;
    compacted.firstInstance = objectIndex;
    uOutputDraws[outputIndex] = compacted;
}

void appendMergedDraw(DrawIndexedIndirectCommand source, uint instanceCount)
{
    uint outputIndex;
    InterlockedAdd(uOutputCount[0], 1, outputIndex);
    if (outputIndex >= pc.outputCapacity)
    {
        return;
    }

    DrawIndexedIndirectCommand compacted = source;
    compacted.instanceCount = instanceCount;
    uOutputDraws[outputIndex] = compacted;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint drawIndex = dispatchThreadId.x;
    if (drawIndex >= pc.inputDrawCount)
    {
        return;
    }

    const DrawIndexedIndirectCommand source = uInputDraws[drawIndex];
    const uint instanceCount = max(source.instanceCount, 1u);
    bool anyResident = false;
    bool allResident = true;
    for (uint instanceIndex = 0u; instanceIndex < instanceCount; ++instanceIndex)
    {
        const uint objectIndex = source.firstInstance + instanceIndex;
        const bool resident = objectIsResident(objectIndex);
        anyResident = anyResident || resident;
        allResident = allResident && resident;
    }
    if (!anyResident)
    {
        return;
    }
    if (allResident)
    {
        appendMergedDraw(source, instanceCount);
        return;
    }

    for (uint instanceIndex = 0u; instanceIndex < instanceCount; ++instanceIndex)
    {
        const uint objectIndex = source.firstInstance + instanceIndex;
        if (objectIsResident(objectIndex))
        {
            appendCompactedDraw(source, objectIndex);
        }
    }
}
