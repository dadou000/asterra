#[compute]
#version 450

// Final reduction over workgroup partials. Fixed-domain Phase 2 domains are
// small enough that one invocation is sufficient; sparse Phase 3 will replace
// this with a hierarchical tile reduction.
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer Partials {
    float partial_depth_sum[];
};
layout(set = 0, binding = 1, std430) writeonly buffer Result {
    float volume_m3;
} result;
layout(set = 0, binding = 2, std430) readonly buffer Params {
    vec4 dims; // width, height, dx, partial_count
} params;

void main() {
    uint count = uint(params.dims.w + 0.5);
    float depth_sum = 0.0;
    for (uint i = 0u; i < count; ++i) depth_sum += partial_depth_sum[i];
    float area = params.dims.z * params.dims.z;
    result.volume_m3 = depth_sum * area;
}
