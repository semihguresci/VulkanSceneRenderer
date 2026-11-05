#include "lighting_structs.slang"
#include "push_constants_common.slang"
#include "object_data_common.slang"
#include "material_data_common.slang"

[[vk::binding(0, 0)]] ConstantBuffer<CameraBuffer> uCamera;
[[vk::binding(1, 0)]] StructuredBuffer<ObjectBuffer> uObjects;
[[vk::binding(2, 0)]] SamplerState materialSamplers[];
[[vk::binding(3, 0)]] StructuredBuffer<GpuMaterial> uMaterials;
[[vk::binding(4, 0)]] StructuredBuffer<GpuTextureMetadata> uTextureMetadata;
[[vk::binding(5, 0)]] Texture2D materialTextures[MATERIAL_TEXTURE_DESCRIPTOR_COUNT];

#include "scene_clip_common.slang"
#include "object_index_common.slang"
#include "pbr_material_common.slang"

[[vk::push_constant]]
ConstantBuffer<NormalValidationPushConstants> pc;

static const uint kNormalValidationInvertFaceClassification = 0x1u;
static const uint kNormalValidationBothSidesValid = 0x2u;

struct VSInput
{
    [[vk::location(0)]] float3 inPosition;
    [[vk::location(2)]] float2 inTexCoord;
    [[vk::location(3)]] float3 inNormal;
};

struct VSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos;
};

struct GSOutput
{
    float4 pos : SV_Position;
    [[vk::location(0)]] float3 worldPos;
};

[shader("vertex")]
VSOutput vertMain(VSInput input, uint instanceID : SV_InstanceID,
                  uint startInstance : SV_StartInstanceLocation)
{
    VSOutput output;
    uint objectIndex = ResolveObjectIndex(pc.objectIndex, instanceID, startInstance);
    ObjectBuffer obj = uObjects[objectIndex];
    GpuMaterial material = uMaterials[obj.objectInfo.x];
    float3 objectPosition = ApplyHeightDisplacement(
        input.inPosition, input.inNormal, input.inTexCoord,
        material.heightTextureIndex, material.heightTextureTransform,
        material.heightScale,
        material.heightOffset);
    float4 worldPos = mul(obj.model, float4(objectPosition, 1.0));
    output.pos = mul(uCamera.viewProj, worldPos);
    output.worldPos = worldPos.xyz;
    return output;
}

[shader("geometry")]
[maxvertexcount(3)]
void geomMain(triangle VSOutput input[3], inout TriangleStream<GSOutput> stream)
{
    const float faceAlpha = saturate(pc.faceAlpha);
    if (pc.showFaceFill != 0u && faceAlpha > 0.001)
    {
        GSOutput out0;
        out0.pos = input[0].pos;
        out0.worldPos = input[0].worldPos;
        stream.Append(out0);

        GSOutput out1;
        out1.pos = input[1].pos;
        out1.worldPos = input[1].worldPos;
        stream.Append(out1);

        GSOutput out2;
        out2.pos = input[2].pos;
        out2.worldPos = input[2].worldPos;
        stream.Append(out2);

        stream.RestartStrip();
    }
}

[shader("fragment")]
float4 fragMain(GSOutput input, bool isFrontFace : SV_IsFrontFace) : SV_Target0
{
    if (SceneClipPlanesClip(input.worldPos, pc.sectionPlaneEnabled, pc.sectionPlane))
    {
        discard;
    }

    const bool bothSidesValid =
        (pc.faceClassificationFlags & kNormalValidationBothSidesValid) != 0u;
    const bool invertFaceClassification =
        (pc.faceClassificationFlags & kNormalValidationInvertFaceClassification) != 0u;
    const bool rasterExpectedFrontFace =
        invertFaceClassification ? !isFrontFace : isFrontFace;
    const bool expectedFrontFace = bothSidesValid || rasterExpectedFrontFace;
    const float3 faceColor =
        expectedFrontFace ? float3(0.0, 0.2, 1.0) : float3(1.0, 0.0, 0.0);
    return float4(faceColor, saturate(pc.faceAlpha));
}


