#[compute]
#version 450

// After a GPU-determined number of adaptive ping-pong substeps, canonicalize the
// final state back into the buffer that was authoritative when advance() began.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer OtherState {
    vec4 other_cells[];
};
layout(set = 0, binding = 1, std430) buffer CanonicalState {
    vec4 canonical_cells[];
};
layout(set = 0, binding = 2, std430) readonly buffer Control {
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
    float current_dt;
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
    uint reserved0;
    uint reserved1;
} control;
layout(set = 0, binding = 3, std430) readonly buffer Params {
    vec4 grid_dt;
    vec4 physics;
    vec4 schedule;
} params;

void main() {
    if ((control.steps_taken & 1u) == 0u) return;
    uvec2 p = gl_GlobalInvocationID.xy;
    uint w = uint(params.grid_dt.x + 0.5);
    uint hgt = uint(params.grid_dt.y + 0.5);
    if (p.x >= w || p.y >= hgt) return;
    uint i = p.y * w + p.x;
    canonical_cells[i] = other_cells[i];
}
