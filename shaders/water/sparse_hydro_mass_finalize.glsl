#[compute]
#version 450

// Final Phase 3 diagnostic reduction. The first pass already converted depth to
// physical volume, so this stage only sums workgroup partials and writes one
// four-byte result for asynchronous readback.
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer Partials {
    float partial_volume_m3[];
};
layout(set = 0, binding = 1, std430) writeonly buffer Result {
    float volume_m3;
} result;
layout(set = 0, binding = 2, std430) readonly buffer Params {
    uvec4 dims; // total_cells, cells_per_tile, capacity, partial_count
    vec4 metric;
} params;

void main() {
    float total = 0.0;
    for (uint i = 0u; i < params.dims.w; ++i) {
        total += partial_volume_m3[i];
    }
    result.volume_m3 = total;
}
