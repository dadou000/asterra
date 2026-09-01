#[compute]
#version 450

// Converts the pre-step characteristic-speed reduction into a safe GPU-side
// substep schedule. If the configured substep cap is insufficient, simulation
// advances only the CFL-safe amount of time and reports cfl_clamped=1 instead of
// violating stability.

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
    float sub_dt;
    float advanced_dt;
    float cfl_dt;

    uint substeps;
    uint max_substeps;
    uint cfl_clamped;
    uint reserved;
} control;
layout(set = 0, binding = 1, std430) readonly buffer Params {
    vec4 grid_dt;  // width, height, dx, requested macro dt
    vec4 physics;  // gravity, dry eps, Manning n, CFL
    vec4 schedule; // max substeps, reserved...
} params;

void main() {
    float requested = max(params.grid_dt.w, 0.0);
    float dx = max(params.grid_dt.z, 1e-4);
    float cfl = clamp(params.physics.w, 0.01, 0.95);
    float max_speed = uintBitsToFloat(control.pre_max_speed_bits);
    uint cap = max(uint(params.schedule.x + 0.5), 1u);

    float safe_dt = requested;
    if (max_speed > 1e-7) {
        safe_dt = cfl * dx / max_speed;
    }
    safe_dt = max(safe_dt, 1e-7);

    uint required = requested > 0.0
        ? max(uint(ceil(requested / safe_dt)), 1u)
        : 1u;

    control.requested_dt = requested;
    control.cfl_dt = safe_dt;
    control.max_substeps = cap;
    control.cfl_clamped = required > cap ? 1u : 0u;
    control.substeps = min(required, cap);

    if (requested <= 0.0) {
        control.sub_dt = 0.0;
        control.advanced_dt = 0.0;
        return;
    }

    if (required <= cap) {
        control.sub_dt = requested / float(control.substeps);
        control.advanced_dt = requested;
    } else {
        // Stability wins over wall-clock catch-up. A later scheduler may carry the
        // unadvanced remainder forward, but this fixed-domain prototype never
        // takes an intentionally super-CFL step.
        control.sub_dt = safe_dt;
        control.advanced_dt = safe_dt * float(cap);
    }
}
