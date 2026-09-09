#[compute]
#version 450

// Reduce per-cell exact external source application accumulated by the sparse SWE
// step. Each cell stores gross added/removed physical volume [m^3] for the current
// macro advance. The buffer is cleared before every advance and written by exactly
// one invocation per cell on each substep, so no float atomics are required.
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer CellLedger {
    vec2 cell_flux_m3[]; // x=added, y=removed
};
layout(set = 0, binding = 1, std430) writeonly buffer Partials {
    vec2 partial_flux_m3[];
};
layout(set = 0, binding = 2, std430) readonly buffer Params {
    uvec4 config; // total_cells, cells_per_tile, capacity, partial_count
} params;

shared vec2 local_sum[256];

void main() {
    uint lane = gl_LocalInvocationIndex;
    uint i = gl_GlobalInvocationID.x;
    vec2 value = vec2(0.0);
    if (i < params.config.x) {
        value = cell_flux_m3[i];
        if (any(isnan(value)) || any(isinf(value))) value = vec2(0.0);
        value = max(value, vec2(0.0));
    }

    local_sum[lane] = value;
    barrier();
    for (uint stride = 128u; stride > 0u; stride >>= 1u) {
        if (lane < stride) local_sum[lane] += local_sum[lane + stride];
        barrier();
    }
    if (lane == 0u && gl_WorkGroupID.x < params.config.w) {
        partial_flux_m3[gl_WorkGroupID.x] = local_sum[0];
    }
}
