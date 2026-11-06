#include "object_data_common.slang"

struct LocalNode
{
    column_major float4x4 local;
    uint4 parentInfo;
};
[[vk::binding(0, 0)]] StructuredBuffer<LocalNode> nodes;
[[vk::binding(1, 0)]] StructuredBuffer<uint> order;
struct WorldMatrix { column_major float4x4 matrix; };
[[vk::binding(2, 0)]] RWStructuredBuffer<WorldMatrix> worlds;
[[vk::binding(3, 0)]] StructuredBuffer<uint2> objectNodes;
[[vk::binding(4, 0)]] RWStructuredBuffer<ObjectBuffer> objects;
[[vk::binding(5, 0)]] ByteAddressBuffer metadata;

struct PushConstants { uint phase; uint first; uint count; uint unused; };
[[vk::push_constant]] ConstantBuffer<PushConstants> pc;

float4x4 loadMetadataMatrix(uint offset)
{
    float4 c0=asfloat(metadata.Load4(offset));
    float4 c1=asfloat(metadata.Load4(offset+16));
    float4 c2=asfloat(metadata.Load4(offset+32));
    float4 c3=asfloat(metadata.Load4(offset+48));
    return transpose(float4x4(c0,c1,c2,c3));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= pc.count) return;
    uint index = pc.first + tid.x;
    if (pc.phase == 0)
    {
        uint node = order[index];
        uint parent = nodes[node].parentInfo.x;
        // GLM/glTF column vectors in a right-handed world, with no Y flip.
        // Slang may eagerly evaluate both operands of a ternary expression.
        // Keep the root sentinel out of every storage-buffer address.
        [branch] if (parent == 0xffffffff)
            worlds[node].matrix = nodes[node].local;
        else
            worlds[node].matrix = mul(worlds[parent].matrix, nodes[node].local);
    }
    else
    {
        uint2 binding = objectNodes[index];
        uint offset=index*112;
        objects[index].objectInfo=metadata.Load4(offset);
        objects[index].boundingSphere=asfloat(metadata.Load4(offset+16));
        objects[index].previousModel=loadMetadataMatrix(offset+32);
        objects[index].temporalInfo=metadata.Load4(offset+96);
        uint node = binding.x;
        [branch] if (node == 0xffffffff)
        {
            objects[index].model=loadMetadataMatrix(binding.y);
            objects[index].normalMatrix0=asfloat(metadata.Load4(binding.y+64));
            objects[index].normalMatrix1=asfloat(metadata.Load4(binding.y+80));
            objects[index].normalMatrix2=asfloat(metadata.Load4(binding.y+96));
            return;
        }
        float4x4 model = worlds[node].matrix;
        float3 a = float3(model[0][0], model[1][0], model[2][0]);
        float3 b = float3(model[0][1], model[1][1], model[2][1]);
        float3 c = float3(model[0][2], model[1][2], model[2][2]);
        float determinant = dot(a, cross(b, c));
        objects[index].model = model;
        // Cofactors are the columns of inverse-transpose, including mirrors.
        objects[index].normalMatrix0 = float4(cross(b, c) / determinant, 0);
        objects[index].normalMatrix1 = float4(cross(c, a) / determinant, 0);
        objects[index].normalMatrix2 = float4(cross(a, b) / determinant, 0);
    }
}
