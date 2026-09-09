#[compute]
#version 450

// Compact single-slot water-volume reduction for conservative fine->coarse
// collapse. Only the requested currently-occupied atlas slot contributes; stale
// bytes in released/unoccupied slots are never authoritative.
layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer State {
    vec4 cells[]; // h, hu, hv, bed
};
layout(set = 0, binding = 1, std430) readonly buffer Occupancy {
    int occupied[];
};
layout(set = 0, binding = 2, std430) writeonly buffer Partials {
    float partial_volume_m3[];
};
layout(set = 0, binding = 3, std430) readonly buffer Params {
    uvec4 dims;   // cells_per_tile, capacity, requested_slot, partial_count
    vec4 metric;  // dx, unused, unused, unused
} params;

shared float local_sum[256];

void main() {
    uint lane = gl_LocalInvocationIndex;
    uint local_i = gl_GlobalInvocationID.x;
    float volume = 0.0;

    uint cells_per_tile = params.dims.x;
    uint capacity = params.dims.y;
    uint slot = params.dims.z;
    if (local_i < cells_per_tile && slot < capacity && occupied[slot] != 0) {
        uint global_i = slot * cells_per_tile + local_i;
        vec4 s = cells[global_i];
        if (!any(isnan(s)) && !any(isinf(s))) {
            float depth = max(s.x, 0.0);
            float dx = max(params.metric.x, 0.0);
            volume = depth * dx * dx;
        }
    }

    local_sum[lane] = volume;
    barrier();

    for (uint stride = 128u; stride > 0u; stride >>= 1u) {
        if (lane < stride) local_sum[lane] += local_sum[lane + stride];
        barrier();
    }

    if (lane == 0u && gl_WorkGroupID.x < params.dims.w) {
        partial_volume_m3[gl_WorkGroupID.x] = local_sum[0];
    }
}
