#[compute]
#version 450

// Serial final reduction over workgroup partials. Fixed-domain Phase 2 domains
// are small enough that this is negligible; sparse Phase 3 will replace it with
// a hierarchical tile reduction.
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer Partials {
    float partial_depth_sum[];
};
layout(set = 0, binding = 1, std430) buffer Control {
    uint pre_max_speed_bits;
    uint pre_max_depth_bits;
    uint pre_wet_count;
    uint pre_invalid_count;
    uint post_max_speed_bits;
    uint post_max_depth_bits;
    uint post_wet_count;
    uint post_invalid_count;
    float requested_dt;
    float remaining_dt;
    float last_sub_dt;
    float advanced_dt;
    float min_cfl_dt;
    float last_cfl_dt;
    uint steps_taken;
    uint max_substeps;
    uint cfl_clamped;
    uint iteration_active;
    uint iter_max_speed_bits;
    uint iter_max_depth_bits;
    uint iter_wet_count;
    uint iter_invalid_count;
    float pre_volume_m3;
    float post_volume_m3;
    float reserved0;
    float reserved1;
} control;
layout(set = 0, binding = 2, std430) readonly buffer Params {
    vec4 grid_dt;
    vec4 physics;
    vec4 schedule; // x=max substeps, y=partial count
} params;

layout(push_constant, std430) uniform MassPush {
    uint mode; // 0 pre, 1 post
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

void main() {
    uint count = uint(params.schedule.y + 0.5);
    float depth_sum = 0.0;
    for (uint i = 0u; i < count; ++i) depth_sum += partial_depth_sum[i];
    float area = params.grid_dt.z * params.grid_dt.z;
    float volume = depth_sum * area;
    if (pc.mode == 0u) control.pre_volume_m3 = volume;
    else control.post_volume_m3 = volume;
}
