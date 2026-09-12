#[compute]
#version 450

// Phase 3 representation-wide water-volume diagnostic.
//
// One invocation corresponds to one conservative sparse state cell. Cells whose
// atlas slot is not currently occupied are ignored even if recycled slots retain
// stale bytes. Each 256-thread workgroup emits one partial *volume* sum [m^3], so
// the final reduction is independent of grid dimensions and only needs the
// partial count.
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
    uvec4 dims;   // total_cells, cells_per_tile, capacity, partial_count
    vec4 metric; // dx, unused, unused, unused
} params;

shared float local_sum[256];

void main() {
    uint lane = gl_LocalInvocationIndex;
    uint i = gl_GlobalInvocationID.x;
    float volume = 0.0;

    if (i < params.dims.x && params.dims.y > 0u) {
        uint slot = i / params.dims.y;
        if (slot < params.dims.z && occupied[slot] != 0) {
            vec4 s = cells[i];
            if (!any(isnan(s)) && !any(isinf(s))) {
                float depth = max(s.x, 0.0);
                float dx = max(params.metric.x, 0.0);
                volume = depth * dx * dx;
            }
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
