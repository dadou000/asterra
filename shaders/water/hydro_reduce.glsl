#[compute]
#version 450

// Workgroup diagnostic reduction for the fixed-domain SWE prototype.
// Mode 0 writes per-iteration scratch used by the adaptive CFL scheduler.
// Mode 1 writes final post-step diagnostics. The first iteration's scratch is
// copied into the persistent pre-step fields by hydro_prepare_step.

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
layout(set = 0, binding = 2, std430) readonly buffer Params {
    vec4 grid_dt;
    vec4 physics;
    vec4 schedule;
} params;

layout(push_constant, std430) uniform ReducePush {
    uint mode;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

shared uint group_max_speed_bits;
shared uint group_max_depth_bits;
shared uint group_wet_count;
shared uint group_invalid_count;

bool invalid_state(vec4 s) {
    return any(isnan(s)) || any(isinf(s)) || s.x < -max(params.physics.y, 1e-8);
}

void main() {
    if (gl_LocalInvocationIndex == 0u) {
        group_max_speed_bits = 0u;
        group_max_depth_bits = 0u;
        group_wet_count = 0u;
        group_invalid_count = 0u;
    }
    barrier();

    uvec2 p = gl_GlobalInvocationID.xy;
    uint w = uint(params.grid_dt.x + 0.5);
    uint hgt = uint(params.grid_dt.y + 0.5);
    bool in_bounds = p.x < w && p.y < hgt;

    if (in_bounds) {
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
        atomicMax(group_max_speed_bits, floatBitsToUint(max(speed, 0.0)));
        atomicMax(group_max_depth_bits, floatBitsToUint(max(depth, 0.0)));
        if (wet) atomicAdd(group_wet_count, 1u);
        if (bad) atomicAdd(group_invalid_count, 1u);
    }

    barrier();
    if (gl_LocalInvocationIndex != 0u) return;

    if (pc.mode == 0u) {
        atomicMax(control.iter_max_speed_bits, group_max_speed_bits);
        atomicMax(control.iter_max_depth_bits, group_max_depth_bits);
        atomicAdd(control.iter_wet_count, group_wet_count);
        atomicAdd(control.iter_invalid_count, group_invalid_count);
    } else {
        atomicMax(control.post_max_speed_bits, group_max_speed_bits);
        atomicMax(control.post_max_depth_bits, group_max_depth_bits);
        atomicAdd(control.post_wet_count, group_wet_count);
        atomicAdd(control.post_invalid_count, group_invalid_count);
    }
}
