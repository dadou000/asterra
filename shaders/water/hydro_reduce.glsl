#[compute]
#version 450

// Per-cell diagnostic reduction for the fixed-domain SWE prototype.
// Positive IEEE754 floats can be reduced with atomicMax on their bit patterns.
// Mode 0 writes the pre-step CFL metrics; mode 1 writes post-step diagnostics.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer State {
    vec4 cells[]; // h, hu, hv, bed
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
    float sub_dt;
    float advanced_dt;
    float cfl_dt;

    uint substeps;
    uint max_substeps;
    uint cfl_clamped;
    uint reserved;
} control;
layout(set = 0, binding = 2, std430) readonly buffer Params {
    vec4 grid_dt;  // width, height, dx, requested macro dt
    vec4 physics;  // gravity, dry eps, Manning n, CFL
    vec4 schedule; // max substeps, reserved...
} params;

layout(push_constant, std430) uniform ReducePush {
    uint mode;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

bool invalid_state(vec4 s) {
    return any(isnan(s)) || any(isinf(s)) || s.x < -max(params.physics.y, 1e-8);
}

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    uint w = uint(params.grid_dt.x + 0.5);
    uint hgt = uint(params.grid_dt.y + 0.5);
    if (p.x >= w || p.y >= hgt) return;

    uint i = p.y * w + p.x;
    vec4 s = cells[i];
    bool bad = invalid_state(s);
    float depth = bad ? 0.0 : max(s.x, 0.0);
    float speed = 0.0;
    bool wet = depth > max(params.physics.y, 1e-8);
    if (wet) {
        vec2 velocity = s.yz / max(depth, 1e-8);
        float c = sqrt(max(params.physics.x, 1e-4) * depth);
        speed = max(abs(velocity.x) + c, abs(velocity.y) + c);
        if (isnan(speed) || isinf(speed)) {
            speed = 0.0;
            bad = true;
        }
    }

    uint speed_bits = floatBitsToUint(max(speed, 0.0));
    uint depth_bits = floatBitsToUint(max(depth, 0.0));
    if (pc.mode == 0u) {
        atomicMax(control.pre_max_speed_bits, speed_bits);
        atomicMax(control.pre_max_depth_bits, depth_bits);
        if (wet) atomicAdd(control.pre_wet_count, 1u);
        if (bad) atomicAdd(control.pre_invalid_count, 1u);
    } else {
        atomicMax(control.post_max_speed_bits, speed_bits);
        atomicMax(control.post_max_depth_bits, depth_bits);
        if (wet) atomicAdd(control.post_wet_count, 1u);
        if (bad) atomicAdd(control.post_invalid_count, 1u);
    }
}
