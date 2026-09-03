#[compute]
#version 450

// Fine-clock temporal HydroLOD scheduler.
//
// H0 advances every CFL tick. Hn normally advances every 2^n H0 ticks using the
// exact accumulated physical dt. The last recorded tick of an advance forces all
// levels to synchronize, preserving the runtime's existing cycle-time contract.

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

layout(set = 0, binding = 1, std430) readonly buffer TemporalParams {
    uvec4 config; // maximum physical lod, enabled, reserved, reserved
} temporal;

layout(push_constant, std430) uniform TemporalPush {
    uint iteration;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

float get_accum(uint lod) {
    if (lod < 4u) return control.temporal_accum_0_3[int(lod)];
    return control.temporal_accum_4_7[int(lod - 4u)];
}

void set_accum(uint lod, float value) {
    if (lod < 4u) control.temporal_accum_0_3[int(lod)] = value;
    else control.temporal_accum_4_7[int(lod - 4u)] = value;
}

void set_step(uint lod, float value) {
    if (lod < 4u) control.temporal_step_0_3[int(lod)] = value;
    else control.temporal_step_4_7[int(lod - 4u)] = value;
}

void main() {
    uint max_lod = min(temporal.config.x, 7u);
    bool enabled = temporal.config.y != 0u;

    if (pc.iteration == 0u) {
        control.temporal_enabled = enabled ? 1u : 0u;
        control.temporal_max_lod = max_lod;
        control.temporal_fine_tick = 0u;
        control.temporal_force_sync = 0u;
        control.temporal_accum_0_3 = vec4(0.0);
        control.temporal_accum_4_7 = vec4(0.0);
        control.temporal_step_0_3 = vec4(0.0);
        control.temporal_step_4_7 = vec4(0.0);
    }

    if (!enabled || control.iteration_active == 0u || control.current_dt <= 0.0)
        return;

    bool final_tick = control.remaining_dt <= 1e-7
        || pc.iteration + 1u >= max(control.max_substeps, 1u);
    control.temporal_force_sync = final_tick ? 1u : 0u;
    control.temporal_step_0_3 = vec4(0.0);
    control.temporal_step_4_7 = vec4(0.0);

    for (uint lod = 0u; lod <= max_lod; ++lod) {
        float accumulated = get_accum(lod) + control.current_dt;
        set_accum(lod, accumulated);
        uint ratio = 1u << lod;
        bool due = final_tick || ((control.temporal_fine_tick + 1u) % ratio) == 0u;
        if (due) set_step(lod, accumulated);
    }
}
