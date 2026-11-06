#pragma once

#if CONTAINER_HAS_NANOVDB
// Portable NanoVDB HLSL traversal. Custom reads bound every access against the
// actual uploaded SSBO; the stock HLSL backend does not check buffer bounds.
#define PNANOVDB_HLSL
#define PNANOVDB_BUF_CUSTOM
#define pnanovdb_buf_t StructuredBuffer<uint>
#define pnanovdb_grid_type_t uint
#define PNANOVDB_GRID_TYPE_GET(grid_typeIn, nameIn) pnanovdb_grid_type_constants[grid_typeIn].nameIn
uint pnanovdb_buf_read_uint32(StructuredBuffer<uint> data, uint byteOffset) {
    uint count, stride;
    data.GetDimensions(count, stride);
    uint word = byteOffset >> 2u;
    return word < count ? data[word] : 0u;
}
uint2 pnanovdb_buf_read_uint64(StructuredBuffer<uint> data, uint byteOffset) {
    return uint2(pnanovdb_buf_read_uint32(data, byteOffset),
                 pnanovdb_buf_read_uint32(data, byteOffset + 4u));
}
void pnanovdb_buf_write_uint32(StructuredBuffer<uint> data, uint byteOffset, uint value) {}
void pnanovdb_buf_write_uint64(StructuredBuffer<uint> data, uint byteOffset, uint2 value) {}
#include <nanovdb/PNanoVDB.h>

float SampleNanoVdb(StructuredBuffer<uint> data, float3 world, float importScale,
                    uint gridByteOffset = 0u) {
    pnanovdb_grid_handle_t grid;
    grid.address = pnanovdb_address_offset(pnanovdb_address_null(), gridByteOffset);
    float3 index = pnanovdb_grid_world_to_indexf(data, grid, world / importScale);
    if (!all(isfinite(index)) || any(abs(index) > 2147483000.0.xxx)) return 0.0;
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(data, pnanovdb_grid_get_tree(data, grid));
    float3 minimum = float3(pnanovdb_root_get_bbox_min(data, root)) - 0.5.xxx;
    float3 maximum = float3(pnanovdb_root_get_bbox_max(data, root)) + 0.5.xxx;
    if (any(index < minimum) || any(index > maximum)) return 0.0;
    int3 base = int3(floor(index));
    float3 fraction = index - float3(base);
    pnanovdb_readaccessor_t accessor;
    pnanovdb_readaccessor_init(accessor, root);
    float value = 0.0;
    [unroll] for (uint corner = 0u; corner < 8u; ++corner) {
        int3 delta = int3(corner & 1u, (corner >> 1u) & 1u, (corner >> 2u) & 1u);
        int3 coordinate = base + delta;
        float3 weight = lerp(1.0.xxx - fraction, fraction, float3(delta));
        pnanovdb_address_t address = pnanovdb_readaccessor_get_value_address(
            PNANOVDB_GRID_TYPE_FLOAT, data, accessor, coordinate);
        value += pnanovdb_read_float(data, address) * weight.x * weight.y * weight.z;
    }
    return isfinite(value) ? clamp(value, 0.0, 1e6) : 0.0;
}

// Conservative empty-tile stepping. Leave an interpolation halo at every tile
// boundary, and never skip a stored leaf, including inactive positive values.
// The returned distance is in world ray units, not index-space units.
float NanoVdbEmptyDistance(StructuredBuffer<uint> data, float3 world,
                           float3 worldDirection, float importScale) {
    pnanovdb_grid_handle_t grid;
    grid.address = pnanovdb_address_null();
    float3 index = pnanovdb_grid_world_to_indexf(data, grid, world / importScale);
    float3 direction = pnanovdb_grid_world_to_index_dirf(data, grid, worldDirection / importScale);
    if (!all(isfinite(index)) || !all(isfinite(direction)) ||
        any(abs(index) > 2147483000.0.xxx)) return 0.0;
    pnanovdb_readaccessor_t accessor;
    pnanovdb_readaccessor_init(accessor,
        pnanovdb_tree_get_root(data, pnanovdb_grid_get_tree(data, grid)));
    int3 coordinate = int3(floor(index));
    uint level;
    pnanovdb_address_t address = pnanovdb_readaccessor_get_value_address_and_level(
        PNANOVDB_GRID_TYPE_FLOAT, data, accessor, coordinate, level);
    if (pnanovdb_read_float(data, address) > 0.0) return 0.0;
    uint dimension = pnanovdb_readaccessor_get_dim(PNANOVDB_GRID_TYPE_FLOAT, data, accessor, coordinate);
    if (dimension <= 1u) return 0.0;
    float3 minimum = floor(index / float(dimension)) * float(dimension) + 1.0.xxx;
    float3 maximum = minimum + float(dimension) - 2.0.xxx;
    if (any(index < minimum) || any(index > maximum)) return 0.0;
    float distance = 1e30;
    [unroll] for (uint axis = 0u; axis < 3u; ++axis) {
        if (direction[axis] > 0.0)
            distance = min(distance, (maximum[axis] - index[axis]) / direction[axis]);
        else if (direction[axis] < 0.0)
            distance = min(distance, (minimum[axis] - index[axis]) / direction[axis]);
    }
    return max(distance, 0.0);
}
#else
float SampleNanoVdb(StructuredBuffer<uint> data, float3 world, float importScale,
                    uint gridByteOffset = 0u) { return 0.0; }
float NanoVdbEmptyDistance(StructuredBuffer<uint> data, float3 world,
                           float3 worldDirection, float importScale) { return 0.0; }
#endif
