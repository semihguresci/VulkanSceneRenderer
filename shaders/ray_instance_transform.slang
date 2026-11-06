#include "object_data_common.slang"
[[vk::binding(0,0)]] StructuredBuffer<ObjectBuffer> objects;
[[vk::binding(1,0)]] ByteAddressBuffer inputs;
[[vk::binding(2,0)]] RWByteAddressBuffer instances;
[[vk::binding(3,0)]] RWByteAddressBuffer surfaces;
struct PushConstants { uint count; uint first; uint hasSurfaces; uint unused; };
[[vk::push_constant]] ConstantBuffer<PushConstants> pc;

[shader("compute")]
[numthreads(64,1,1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    if(tid.x >= pc.count) return;
    uint index=pc.first+tid.x;
    uint input=index*24;
    uint object=inputs.Load(input+16);
    float4 row0, row1, row2;
    // Explicit control flow prevents a CPU-only sentinel from addressing objects.
    [branch] if(object==0xffffffff)
    {
        uint fallback=inputs.Load(input+20);
        row0=asfloat(inputs.Load4(fallback));
        row1=asfloat(inputs.Load4(fallback+16));
        row2=asfloat(inputs.Load4(fallback+32));
    }
    else
    {
        float4x4 model=objects[object].model;
        row0=float4(model[0][0],model[0][1],model[0][2],model[0][3]);
        row1=float4(model[1][0],model[1][1],model[1][2],model[1][3]);
        row2=float4(model[2][0],model[2][1],model[2][2],model[2][3]);
    }
    uint output=index*64;
    // Vulkan AS instances are a row-major 3x4, independent of GLM's layout.
    instances.Store4(output,asuint(row0));
    instances.Store4(output+16,asuint(row1));
    instances.Store4(output+32,asuint(row2));
    instances.Store4(output+48,inputs.Load4(input));
    if(pc.hasSurfaces!=0)
    {
        float3 a=float3(row0.x,row1.x,row2.x);
        float3 b=float3(row0.y,row1.y,row2.y);
        float3 c=float3(row0.z,row1.z,row2.z);
        float determinant=dot(a,cross(b,c));
        uint surface=index*112;
        // Closest-hit matrices use the ordinary column-major glTF convention.
        surfaces.Store4(surface,asuint(float4(a,0)));
        surfaces.Store4(surface+16,asuint(float4(b,0)));
        surfaces.Store4(surface+32,asuint(float4(c,0)));
        surfaces.Store4(surface+48,asuint(float4(row0.w,row1.w,row2.w,1)));
        surfaces.Store4(surface+64,asuint(float4(cross(b,c)/determinant,determinant<0 ? -1.0 : 1.0)));
        surfaces.Store4(surface+80,asuint(float4(cross(c,a)/determinant,0)));
        surfaces.Store4(surface+96,asuint(float4(cross(a,b)/determinant,0)));
    }
}
