// Exercise the actual production closure independently of tone mapping and the
// renderer's final HDR clamp. The helper only requires these medium controls.
struct TestMedium {
    float4 temperatureControls;
    uint4 flags;
};
struct Query {
    float4 albedoTransmittance;
    uint4 controls; // medium index, geometry visibility
};
struct Result {
    float4 gainEnabled;
    float4 source;
};
[[vk::binding(0, 0)]] StructuredBuffer<TestMedium> media;
[[vk::binding(1, 0)]] StructuredBuffer<Query> queries;
[[vk::binding(2, 0)]] RWStructuredBuffer<Result> results;

uint MediumCount() { return 4u; }
#include "openvdb_multiple_scattering.slang"

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 id : SV_DispatchThreadID) {
    uint count, stride;
    queries.GetDimensions(count, stride);
    if (id.x >= count) return;
    Query query = queries[id.x];
    uint index = min(query.controls.x, 3u);
    float3 gain = VolumeMultipleScatteringGain(index, query.albedoTransmittance.rgb,
                                               query.albedoTransmittance.a);
    float3 coefficients[4] = {0.0.xxx, 0.0.xxx, 0.0.xxx, 0.0.xxx};
    coefficients[index] = float3(1, 2, 3);
    Result result;
    result.gainEnabled = float4(gain, VolumeMultipleScatteringEnabled() ? 1.0 : 0.0);
    result.source = float4(VolumeMultipleScatteringSource(coefficients,
        query.albedoTransmittance.rgb, query.albedoTransmittance.a,
        float(query.controls.y)), 0.0);
    results[id.x] = result;
}
