// shadow_cull.slang - Compute shader for per-cascade light-space shadow caster culling.

#include "lighting_structs.slang"
#include "draw_indirect_common.slang"

#include "object_data_common.slang"

[[vk::push_constant]]
ConstantBuffer<ShadowCullPushConstants> pc;

[[vk::binding(0, 0)]] ConstantBuffer<ShadowCullBuffer> uShadowCull;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;
[[vk::binding(2, 0)]] StructuredBuffer<DrawIndexedIndirectCommand> uInputDraws;
[[vk::binding(3, 0)]] RWStructuredBuffer<DrawIndexedIndirectCommand> uOutputDraws;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint> uDrawCount;

bool SphereIntersectsCascade(float4 sphere, ShadowCascadeCullData cascade)
{
	if (!all(isfinite(sphere)) || sphere.w <= 0.0)
	{
		return true;
	}

	float3 center = mul(cascade.lightView, float4(sphere.xyz, 1.0)).xyz;
	float radius = sphere.w;
	if (!all(isfinite(center)) || !isfinite(radius) || radius <= 0.0)
	{
		return true;
	}

	float3 sphereMin = center - radius.xxx;
	float3 sphereMax = center + radius.xxx;
	if (!all(isfinite(sphereMin)) || !all(isfinite(sphereMax)) ||
		!all(isfinite(cascade.casterMinBounds)) ||
		!all(isfinite(cascade.casterMaxBounds)))
	{
		return true;
	}

	return sphereMax.x >= cascade.casterMinBounds.x &&
		   sphereMin.x <= cascade.casterMaxBounds.x &&
		   sphereMax.y >= cascade.casterMinBounds.y &&
		   sphereMin.y <= cascade.casterMaxBounds.y &&
		   sphereMax.z >= cascade.casterMinBounds.z &&
		   sphereMin.z <= cascade.casterMaxBounds.z;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 dtid : SV_DispatchThreadID)
{
	uint idx = dtid.x;
	if (idx >= pc.drawCount || pc.cascadeIndex >= SHADOW_CASCADE_COUNT)
	{
		return;
	}

	DrawIndexedIndirectCommand draw = uInputDraws[idx];
	if (idx == 0)
	{
		uDrawCount[0] = pc.drawCount;
	}

	DrawIndexedIndirectCommand outputDraw = draw;
	uint objectIndex = draw.firstInstance;
	if (objectIndex >= pc.objectCount)
	{
		outputDraw.instanceCount = 0;
		uOutputDraws[pc.outputOffset + idx] = outputDraw;
		return;
	}
	ObjectBuffer objectData = uObjects[objectIndex];
	float4 boundingSphere = objectData.boundingSphere;

	bool visible = !all(isfinite(boundingSphere)) ||
				   boundingSphere.w <= 0.0 ||
				   SphereIntersectsCascade(
					   boundingSphere,
					   uShadowCull.cascades[pc.cascadeIndex]);

	if (visible)
	{
		uOutputDraws[pc.outputOffset + idx] = outputDraw;
	}
	else
	{
		outputDraw.instanceCount = 0;
		uOutputDraws[pc.outputOffset + idx] = outputDraw;
	}
}
