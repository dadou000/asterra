#[compute]
#version 450

// Finalize one fine-clock HydroLOD tick after SWE/reflux/canonicalization.

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

    uint iter_max_cfl_rate_bits;
    uint reserved2;

    uint temporal_enabled;
    uint temporal_max_lod;
    uint temporal_fine_tick;
    uint temporal_force_sync;
    vec4 temporal_accum_0_3;
    vec4 temporal_accum_4_7;
    vec4 temporal_step_0_3;
    vec4 temporal_step_4_7;
} control;

float get_step(uint lod) {
    if (lod < 4u) return control.temporal_step_0_3[int(lod)];
    return control.temporal_step_4_7[int(lod - 4u)];
}

void set_accum(uint lod, float value) {
    if (lod < 4u) control.temporal_accum_0_3[int(lod)] = value;
    else control.temporal_accum_4_7[int(lod - 4u)] = value;
}

void main() {
    if (control.temporal_enabled == 0u || control.iteration_active == 0u)
        return;

    uint max_lod = min(control.temporal_max_lod, 7u);
    for (uint lod = 0u; lod <= max_lod; ++lod) {
        if (get_step(lod) > 0.0) set_accum(lod, 0.0);
    }

    if (control.temporal_force_sync != 0u)
        control.temporal_fine_tick = 0u;
    else
        control.temporal_fine_tick += 1u;
}
