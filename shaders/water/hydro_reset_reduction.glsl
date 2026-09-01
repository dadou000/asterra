#[compute]
#version 450

// Clears only the per-iteration reduction scratch. The rest of the scheduling
// state survives between candidate substeps inside one macro advance.

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) buffer Control {
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

void main() {
    control.current_dt = 0.0;
    control.iteration_active = 0u;
    control.iter_max_speed_bits = 0u;
    control.iter_max_depth_bits = 0u;
    control.iter_wet_count = 0u;
    control.iter_invalid_count = 0u;
}
