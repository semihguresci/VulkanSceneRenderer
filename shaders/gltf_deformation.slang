// Morph displacements precede linear-blend skinning. All buffers use explicit
// byte offsets so the output exactly matches the 72-byte C++ Vertex stream.
// Matrices arrive in glTF/GLM column-major order; columns are loaded explicitly
// and positions are multiplied as column vectors without any transposition.

[[vk::binding(0, 0)]] ByteAddressBuffer sources;
[[vk::binding(1, 0)]] ByteAddressBuffer poses;
[[vk::binding(2, 0)]] RWByteAddressBuffer currentVertices;
[[vk::binding(3, 0)]] RWByteAddressBuffer previousVertices;

struct PushConstants
{
    uint vertexCount;
    uint mapOffset;
    uint jobsOffset;
    uint baseVertex;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> pc;

float3 unitOr(float3 value, float3 fallback)
{
    float magnitude = max(max(abs(value.x), abs(value.y)), abs(value.z));
    if (!(magnitude > 0.0) || !all(isfinite(value))) return fallback;
    float3 scaled = value / magnitude;
    return scaled * rsqrt(dot(scaled, scaled));
}

float3 frameTangent(float3 tangent, float3 normal)
{
    float3 axis = abs(normal.x) < 0.9 ? float3(1, 0, 0) : float3(0, 1, 0);
    float3 fallback = normalize(axis - normal * dot(normal, axis));
    return unitOr(tangent - normal * dot(normal, tangent), fallback);
}

struct DeformedVertex
{
    float3 position;
    float3 normal;
    float4 tangent;
};

DeformedVertex evaluate(uint vertex, uint4 mapping, uint4 job,
                         uint paletteOffset, uint paletteCount,
                         uint weightsOffset)
{
    uint vertexOffset = vertex * 72;
    float3 bindNormal = asfloat(sources.Load3(vertexOffset + 40));
    DeformedVertex result;
    result.position = asfloat(sources.Load3(vertexOffset));
    result.normal = bindNormal;
    result.tangent = asfloat(sources.Load4(vertexOffset + 52));

    for (uint target = 0; target < job.y; ++target)
    {
        // A target stores nine tightly packed floats per vertex: P, N, T.
        uint deltaOffset = job.x + (target * job.z + mapping.y) * 36;
        float weight = asfloat(poses.Load(weightsOffset + target * 4));
        result.position += weight * asfloat(sources.Load3(deltaOffset));
        result.normal += weight * asfloat(sources.Load3(deltaOffset + 12));
        result.tangent.xyz += weight * asfloat(sources.Load3(deltaOffset + 24));
    }
    float handedness = result.tangent.w < 0.0 ? -1.0 : 1.0;
    if (paletteCount != 0 && mapping.w != 0)
    {
        float4 c0 = 0.0;
        float4 c1 = 0.0;
        float4 c2 = 0.0;
        float4 c3 = 0.0;
        float weightSum = 0.0;
        for (uint influence = 0; influence < mapping.w; ++influence)
        {
            uint influenceOffset = mapping.z + influence * 8;
            uint joint = sources.Load(influenceOffset);
            float weight = asfloat(sources.Load(influenceOffset + 4));
            uint matrixOffset = paletteOffset + joint * 64;
            c0 += weight * asfloat(poses.Load4(matrixOffset));
            c1 += weight * asfloat(poses.Load4(matrixOffset + 16));
            c2 += weight * asfloat(poses.Load4(matrixOffset + 32));
            c3 += weight * asfloat(poses.Load4(matrixOffset + 48));
            weightSum += weight;
        }
        c0 /= weightSum;
        c1 /= weightSum;
        c2 /= weightSum;
        c3 /= weightSum;
        result.position = (c0 * result.position.x + c1 * result.position.y +
                              c2 * result.position.z + c3).xyz;
        float3 a = c0.xyz;
        float3 b = c1.xyz;
        float3 c = c2.xyz;
        float magnitude = max(max(max(abs(a.x), abs(a.y)), abs(a.z)),
            max(max(max(abs(b.x), abs(b.y)), abs(b.z)),
                max(max(abs(c.x), abs(c.y)), abs(c.z))));
        if (magnitude > 0.0)
        {
            a /= magnitude;
            b /= magnitude;
            c /= magnitude;
        }
        float determinant = dot(a, cross(b, c));
        if (determinant != 0.0)
        {
            // Cofactors are the inverse-transpose times determinant. Safe
            // normalization removes its magnitude; retain its mirror sign.
            result.normal = (cross(b, c) * result.normal.x +
                cross(c, a) * result.normal.y + cross(a, b) * result.normal.z) *
                (determinant < 0.0 ? -1.0 : 1.0);
        }
        else
        {
            result.normal = a * result.normal.x + b * result.normal.y + c * result.normal.z;
        }
        result.tangent.xyz = a * result.tangent.x + b * result.tangent.y + c * result.tangent.z;
        handedness *= determinant < 0.0 ? -1.0 : 1.0;
    }
    result.normal = unitOr(result.normal, unitOr(bindNormal, float3(0, 1, 0)));
    result.tangent = float4(frameTangent(result.tangent.xyz, result.normal), handedness);
    return result;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void computeMain(uint3 thread : SV_DispatchThreadID)
{
    uint vertex = pc.baseVertex + thread.x;
    if (vertex >= pc.vertexCount || vertex < pc.baseVertex) return;
    uint outputOffset = vertex * 72;
    // Preserve colors, both UV sets, and all remaining bind bytes exactly.
    for (uint word = 0; word < 18; ++word)
    {
        uint value = sources.Load(outputOffset + word * 4);
        currentVertices.Store(outputOffset + word * 4, value);
        previousVertices.Store(outputOffset + word * 4, value);
    }
    uint4 mapping = sources.Load4(pc.mapOffset + vertex * 16);
    if (mapping.x == 0xffffffffu) return;
    uint4 job = sources.Load4(pc.jobsOffset + mapping.x * 16);
    uint4 frame0 = poses.Load4(mapping.x * 48);
    uint4 frame1 = poses.Load4(mapping.x * 48 + 16);
    uint4 evaluated = poses.Load4(mapping.x * 48 + 32);
    // Exceptional missing-base-frame primitives are evaluated on the CPU with
    // flat-normal/MikkTSpace regeneration. Preserve their complete final ABI
    // through the same mapped upload and compute visibility path.
    if (evaluated.z != 0u)
    {
        for (uint word = 0; word < 18; ++word)
            currentVertices.Store(outputOffset + word * 4,
                poses.Load(evaluated.x + mapping.y * 72 + word * 4));
    }
    else
    {
        DeformedVertex current = evaluate(vertex, mapping, job, frame0.x, frame1.x, frame0.z);
        currentVertices.Store3(outputOffset, asuint(current.position));
        currentVertices.Store3(outputOffset + 40, asuint(current.normal));
        currentVertices.Store4(outputOffset + 52, asuint(current.tangent));
    }
    if (frame1.z != 0u)
    {
        for (uint word = 0; word < 18; ++word)
            previousVertices.Store(outputOffset + word * 4,
                currentVertices.Load(outputOffset + word * 4));
    }
    else if (evaluated.w != 0u)
    {
        for (uint word = 0; word < 18; ++word)
            previousVertices.Store(outputOffset + word * 4,
                poses.Load(evaluated.y + mapping.y * 72 + word * 4));
    }
    else
    {
        DeformedVertex previous = evaluate(vertex, mapping, job, frame0.y, frame1.y, frame0.w);
        previousVertices.Store3(outputOffset, asuint(previous.position));
        previousVertices.Store3(outputOffset + 40, asuint(previous.normal));
        previousVertices.Store4(outputOffset + 52, asuint(previous.tangent));
    }
}
