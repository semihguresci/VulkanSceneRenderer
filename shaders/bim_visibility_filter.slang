struct BimVisibilityObjectMetadata
{
    uint4 semanticIds;   // x type, y storey, z material, w discipline
    uint4 propertyIds;   // x phase, y fire rating, z load-bearing, w status
    uint4 identity;      // x product identity, y object index, z geometry kind, w flags
};

struct BimVisibilityFilterPushConstants
{
    uint objectCount;
    uint flags;
    uint drawBudgetMaxObjects;
    uint selectedObjectIndex;
    uint typeId;
    uint storeyId;
    uint materialId;
    uint disciplineId;
    uint phaseId;
    uint fireRatingId;
    uint loadBearingId;
    uint statusId;
    uint selectedProductId;
    uint pad0;
    uint pad1;
    uint pad2;
};

static const uint BIM_VIS_FILTER_TYPE = 1u << 0u;
static const uint BIM_VIS_FILTER_STOREY = 1u << 1u;
static const uint BIM_VIS_FILTER_MATERIAL = 1u << 2u;
static const uint BIM_VIS_FILTER_DISCIPLINE = 1u << 3u;
static const uint BIM_VIS_FILTER_PHASE = 1u << 4u;
static const uint BIM_VIS_FILTER_FIRE_RATING = 1u << 5u;
static const uint BIM_VIS_FILTER_LOAD_BEARING = 1u << 6u;
static const uint BIM_VIS_FILTER_STATUS = 1u << 7u;
static const uint BIM_VIS_FILTER_DRAW_BUDGET = 1u << 8u;
static const uint BIM_VIS_FILTER_ISOLATE_SELECTION = 1u << 9u;
static const uint BIM_VIS_FILTER_HIDE_SELECTION = 1u << 10u;

[[vk::push_constant]]
ConstantBuffer<BimVisibilityFilterPushConstants> pc;

[[vk::binding(0, 0)]] StructuredBuffer<BimVisibilityObjectMetadata> uMetadata;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> uVisibilityMask;

bool filterEnabled(uint flag)
{
    return (pc.flags & flag) != 0u;
}

bool sameSelectedProduct(BimVisibilityObjectMetadata metadata)
{
    if (pc.selectedProductId != 0u && metadata.identity.x != 0u)
    {
        return metadata.identity.x == pc.selectedProductId;
    }
    return metadata.identity.y == pc.selectedObjectIndex;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint objectIndex = dispatchThreadId.x;
    if (objectIndex >= pc.objectCount)
    {
        return;
    }

    const BimVisibilityObjectMetadata metadata = uMetadata[objectIndex];
    bool visible = true;

    if (filterEnabled(BIM_VIS_FILTER_TYPE))
    {
        visible = visible && metadata.semanticIds.x == pc.typeId;
    }
    if (filterEnabled(BIM_VIS_FILTER_STOREY))
    {
        visible = visible && metadata.semanticIds.y == pc.storeyId;
    }
    if (filterEnabled(BIM_VIS_FILTER_MATERIAL))
    {
        visible = visible && metadata.semanticIds.z == pc.materialId;
    }
    if (filterEnabled(BIM_VIS_FILTER_DISCIPLINE))
    {
        visible = visible && metadata.semanticIds.w == pc.disciplineId;
    }
    if (filterEnabled(BIM_VIS_FILTER_PHASE))
    {
        visible = visible && metadata.propertyIds.x == pc.phaseId;
    }
    if (filterEnabled(BIM_VIS_FILTER_FIRE_RATING))
    {
        visible = visible && metadata.propertyIds.y == pc.fireRatingId;
    }
    if (filterEnabled(BIM_VIS_FILTER_LOAD_BEARING))
    {
        visible = visible && metadata.propertyIds.z == pc.loadBearingId;
    }
    if (filterEnabled(BIM_VIS_FILTER_STATUS))
    {
        visible = visible && metadata.propertyIds.w == pc.statusId;
    }

    const bool selected = sameSelectedProduct(metadata);
    if (filterEnabled(BIM_VIS_FILTER_ISOLATE_SELECTION))
    {
        visible = visible && selected;
    }
    if (filterEnabled(BIM_VIS_FILTER_HIDE_SELECTION) && selected)
    {
        visible = false;
    }
    if (filterEnabled(BIM_VIS_FILTER_DRAW_BUDGET))
    {
        visible = visible &&
                  (metadata.identity.y < pc.drawBudgetMaxObjects || selected);
    }

    uVisibilityMask[objectIndex] = visible ? 1u : 0u;
}
