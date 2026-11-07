#pragma once
#include "oit_common.slang"

[[vk::binding(16, 0)]] [[vk::image_format("r32ui")]] RWTexture2D<uint> volumeOitHeads;
[[vk::binding(17, 0)]] StructuredBuffer<TransparentNode> volumeOitNodes;
[[vk::binding(18, 0)]] ConstantBuffer<OitMetadataBuffer> volumeOitMetadata;

struct VolumeTransparentLayers {
    uint indices[MAX_OIT_NODES_PER_PIXEL];
    uint count;
    uint cursor;
};

float VolumeOitFinite(float value, float fallback) { return isfinite(value) ? value : fallback; }

VolumeTransparentLayers LoadVolumeTransparentLayers(uint2 pixel) {
    VolumeTransparentLayers layers;
    layers.count = 0u;
    layers.cursor = 0u;
    if ((pc.extentFlags.w & 512u) == 0u ||
        pixel.x >= volumeOitMetadata.viewportWidth || pixel.y >= volumeOitMetadata.viewportHeight)
        return layers;
    uint current = volumeOitHeads[pixel];
    // Match the existing resolver: retain at most the first 64 linked nodes,
    // reject out-of-capacity indices, then sort reversed-Z nearest first.
    [loop] while (current != INVALID_NODE_INDEX && layers.count < MAX_OIT_NODES_PER_PIXEL) {
        if (current >= volumeOitMetadata.nodeCapacity) break;
        TransparentNode node = volumeOitNodes[current];
        float depth = VolumeOitFinite(node.depth, 0.0);
        uint insert = layers.count;
        [loop] while (insert > 0u) {
            float previousDepth = VolumeOitFinite(volumeOitNodes[layers.indices[insert - 1u]].depth, 0.0);
            if (previousDepth >= depth) break;
            layers.indices[insert] = layers.indices[insert - 1u];
            --insert;
        }
        layers.indices[insert] = current;
        ++layers.count;
        current = node.next;
    }
    return layers;
}

void CompositeVolumeLayer(uint index, inout float3 integrated, inout float transmittance) {
    TransparentNode node = volumeOitNodes[index];
    float alpha = saturate(VolumeOitFinite(node.color.a, 0.0));
    float3 color = all(isfinite(node.color.rgb)) ? node.color.rgb : 0.0.xxx;
    integrated += transmittance * alpha * color;
    transmittance *= 1.0 - alpha;
}

void IntegrateVolumePiece(float length, float extinction, float3 source,
                          inout float3 integrated, inout float transmittance,
                          inout bool contributes) {
    if (length <= 0.0 || transmittance <= 0.0) return;
    float thickness = min(extinction * length, 40.0);
    float opacity = thickness < 1e-3 ? thickness * (1.0 - 0.5 * thickness) : 1.0 - exp(-thickness);
    float integral = thickness < 1e-3 ? length * (1.0 - 0.5 * thickness) : opacity / extinction;
    contributes = contributes || opacity > 0.0 || any(source > 0.0.xxx);
    integrated += transmittance * integral * source;
    transmittance *= 1.0 - opacity;
}

// Split only the analytic integral, never the primary sample lattice. Every
// piece reuses the original march sample's coefficients. Each surface event is
// consumed once, so total work is the march budget plus at most 64 boundaries.
void IntegrateVolumeLayeredInterval(float begin, float length, float extinction, float3 source,
                                    float2 uv, float3 origin, float3 direction,
                                    inout VolumeTransparentLayers layers,
                                    inout float3 integrated, inout float transmittance,
                                    inout bool contributes) {
    if (layers.cursor >= layers.count) {
        IntegrateVolumePiece(length, extinction, source, integrated, transmittance, contributes);
        return;
    }
    float end = begin + length;
    float remaining = length;
    [loop] while (layers.cursor < layers.count) {
        TransparentNode node = volumeOitNodes[layers.indices[layers.cursor]];
        float3 surfaceWorld;
        float distance = 1e30;
        if (TryReconstructWorldPosition(uv, saturate(VolumeOitFinite(node.depth, 0.0)),
                                        camera.inverseViewProj, surfaceWorld))
            distance = max(VolumeOitFinite(dot(surfaceWorld - origin, direction), 1e30), 0.0);
        if (distance > end) break;
        float piece = clamp(distance - begin, 0.0, remaining);
        IntegrateVolumePiece(piece, extinction, source, integrated, transmittance, contributes);
        CompositeVolumeLayer(layers.indices[layers.cursor], integrated, transmittance);
        ++layers.cursor;
        begin += piece;
        remaining = max(remaining - piece, 0.0);
    }
    IntegrateVolumePiece(remaining, extinction, source, integrated, transmittance, contributes);
}

void FinishVolumeTransparentLayers(uint2 pixel, inout VolumeTransparentLayers layers,
                                   inout float3 integrated, inout float transmittance) {
    [loop] while (layers.cursor < layers.count) {
        CompositeVolumeLayer(layers.indices[layers.cursor], integrated, transmittance);
        ++layers.cursor;
    }
    // Temporal/post composition must not resolve the same list a second time.
    // Counter/nodes remain intact for overflow handling and resource telemetry.
    if (layers.count > 0u && (pc.extentFlags.w & 512u) != 0u &&
        pixel.x < volumeOitMetadata.viewportWidth && pixel.y < volumeOitMetadata.viewportHeight)
        volumeOitHeads[pixel] = INVALID_NODE_INDEX;
}
