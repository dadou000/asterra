#[compute]
#version 450

// Converts the occupied-atlas characteristic-rate reduction into one CFL-safe
// sparse substep. For mixed HydroLODs the reduction supplies max((|u|+c)/dx_local),
// so one common step is stable for every occupied physical resolution.

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
} control;
layout(set = 0, binding = 1, std430) readonly buffer Params {
    vec4 grid_dt;  // tile_resolution, capacity, H0 dx, requested macro dt
    vec4 physics;  // gravity, dry eps, Manning n, CFL
    vec4 schedule; // max substeps, H0 tile level, HydroLOD enabled, reserved
} params;

layout(push_constant, std430) uniform PreparePush {
    uint iteration;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

void main() {
    float requested = max(params.grid_dt.w, 0.0);
    uint cap = max(uint(params.schedule.x + 0.5), 1u);

    if (pc.iteration == 0u) {
        control.requested_dt = requested;
        control.remaining_dt = requested;
        control.current_dt = 0.0;
        control.advanced_dt = 0.0;
        control.min_cfl_dt = requested;
        control.last_cfl_dt = requested;
        control.steps_taken = 0u;
        control.max_substeps = cap;
        control.cfl_clamped = 0u;

        control.pre_max_speed_bits = control.iter_max_speed_bits;
        control.pre_max_depth_bits = control.iter_max_depth_bits;
        control.pre_wet_count = control.iter_wet_count;
        control.pre_invalid_count = control.iter_invalid_count;
    }

    control.iteration_active = 0u;
    control.current_dt = 0.0;
    if (control.remaining_dt <= 1e-7 || pc.iteration >= cap) return;

    float cfl = clamp(params.physics.w, 0.01, 0.95);
    float max_cfl_rate = uintBitsToFloat(control.iter_max_cfl_rate_bits);
    float safe_dt = control.remaining_dt;
    if (max_cfl_rate > 1e-7) safe_dt = cfl / max_cfl_rate;
    safe_dt = max(safe_dt, 1e-7);

    float step_dt = min(control.remaining_dt, safe_dt);
    control.current_dt = step_dt;
    control.last_cfl_dt = safe_dt;
    control.min_cfl_dt = pc.iteration == 0u
        ? safe_dt : min(control.min_cfl_dt, safe_dt);
    control.iteration_active = 1u;
    control.steps_taken += 1u;
    control.advanced_dt += step_dt;
    control.remaining_dt = max(control.remaining_dt - step_dt, 0.0);

    if (pc.iteration + 1u >= cap && control.remaining_dt > 1e-7)
        control.cfl_clamped = 1u;
}
