#[compute]
#version 450

// One partial depth sum per 8x8 workgroup. This debug/soak diagnostic is separate
// from solver control so volume instrumentation cannot perturb CFL scheduling.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer State {
    vec4 cells[]; // h, hu, hv, bed
};
layout(set = 0, binding = 1, std430) writeonly buffer Partials {
    float partial_depth_sum[];
};
layout(set = 0, binding = 2, std430) readonly buffer Params {
    vec4 dims; // width, height, dx, partial_count
} params;

shared float local_sum[64];

void main() {
    uint lane = gl_LocalInvocationIndex;
    uvec2 p = gl_GlobalInvocationID.xy;
    uint w = uint(params.dims.x + 0.5);
    uint hgt = uint(params.dims.y + 0.5);

    float depth = 0.0;
    if (p.x < w && p.y < hgt) {
        vec4 s = cells[p.y * w + p.x];
        if (!any(isnan(s)) && !any(isinf(s))) depth = max(s.x, 0.0);
    }
    local_sum[lane] = depth;
    barrier();

    for (uint stride = 32u; stride > 0u; stride >>= 1u) {
        if (lane < stride) local_sum[lane] += local_sum[lane + stride];
        barrier();
    }

    if (lane == 0u) {
        uint group_index = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
        partial_depth_sum[group_index] = local_sum[0];
    }
}
