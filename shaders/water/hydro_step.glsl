#[compute]
#version 450

// Phase 2 fixed-domain shallow-water update.
// Mirrors HydroReferenceSolver: conservative h/hu/hv, hydrostatic reconstruction,
// Rusanov interface flux, positivity-preserving wet/dry handling and semi-implicit
// Manning friction. GPU CFL preparation supplies sub_dt/substeps; a push constant
// selects which recorded dispatch in the fixed maximum sequence is active.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer StateIn {
    vec4 cells_in[];  // h, hu, hv, bed elevation
};
layout(set = 0, binding = 1, std430) writeonly buffer StateOut {
    vec4 cells_out[];
};
layout(set = 0, binding = 2, std430) readonly buffer Sources {
    vec4 sources[];   // rain m/s, infiltration m/s, reserved, reserved
};
layout(set = 0, binding = 3, std430) readonly buffer Params {
    vec4 grid_dt;     // width, height, cell_size_m, requested macro dt
    vec4 physics;     // gravity, dry_eps, Manning n, CFL
    vec4 schedule;    // max substeps, reserved...
} params;
layout(set = 0, binding = 4, std430) readonly buffer Control {
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

layout(push_constant, std430) uniform StepPush {
    uint step_index;
    uint pad0;
    uint pad1;
    uint pad2;
} pc;

struct HydroInterface {
    vec3 flux;
    float h_left;
    float h_right;
};

int grid_width() { return int(params.grid_dt.x + 0.5); }
int grid_height() { return int(params.grid_dt.y + 0.5); }
float dx() { return max(params.grid_dt.z, 1e-4); }
float dt() { return max(control.sub_dt, 0.0); }
float grav() { return max(params.physics.x, 1e-4); }
float dry_eps() { return max(params.physics.y, 1e-8); }
float manning_n() { return max(params.physics.z, 0.0); }

int idx(ivec2 p) {
    return p.y * grid_width() + p.x;
}

vec4 cell_at(ivec2 p) {
    ivec2 q = clamp(p, ivec2(0), ivec2(grid_width() - 1, grid_height() - 1));
    return cells_in[idx(q)];
}

vec3 reconstruct(vec3 q, float reconstructed_h) {
    if (q.x <= dry_eps() || reconstructed_h <= dry_eps()) return vec3(0.0);
    float scale = reconstructed_h / q.x;
    return vec3(reconstructed_h, q.yz * scale);
}

vec3 flux_x(vec3 q) {
    if (q.x <= dry_eps()) return vec3(0.0);
    float u = q.y / q.x;
    return vec3(q.y, q.y * u + 0.5 * grav() * q.x * q.x, q.z * u);
}

vec3 flux_y(vec3 q) {
    if (q.x <= dry_eps()) return vec3(0.0);
    float v = q.z / q.x;
    return vec3(q.z, q.y * v, q.z * v + 0.5 * grav() * q.x * q.x);
}

float signal_x(vec3 q) {
    if (q.x <= dry_eps()) return 0.0;
    return abs(q.y / q.x) + sqrt(grav() * q.x);
}

float signal_y(vec3 q) {
    if (q.x <= dry_eps()) return 0.0;
    return abs(q.z / q.x) + sqrt(grav() * q.x);
}

vec3 rusanov_x(vec3 l, vec3 r) {
    float a = max(signal_x(l), signal_x(r));
    return 0.5 * (flux_x(l) + flux_x(r)) - 0.5 * a * (r - l);
}

vec3 rusanov_y(vec3 l, vec3 r) {
    float a = max(signal_y(l), signal_y(r));
    return 0.5 * (flux_y(l) + flux_y(r)) - 0.5 * a * (r - l);
}

HydroInterface interface_x(vec3 l, float zl, vec3 r, float zr) {
    float zstar = max(zl, zr);
    float hl = max(l.x + zl - zstar, 0.0);
    float hr = max(r.x + zr - zstar, 0.0);
    HydroInterface result;
    result.flux = rusanov_x(reconstruct(l, hl), reconstruct(r, hr));
    result.h_left = hl;
    result.h_right = hr;
    return result;
}

HydroInterface interface_y(vec3 lower, float zl, vec3 upper, float zr) {
    float zstar = max(zl, zr);
    float hl = max(lower.x + zl - zstar, 0.0);
    float hr = max(upper.x + zr - zstar, 0.0);
    HydroInterface result;
    result.flux = rusanov_y(reconstruct(lower, hl), reconstruct(upper, hr));
    result.h_left = hl;
    result.h_right = hr;
    return result;
}

vec3 apply_friction(vec3 q, float step_dt) {
    float n = manning_n();
    if (q.x <= 1e-3 || n <= 0.0) return q;
    vec2 velocity = q.yz / q.x;
    float speed = length(velocity);
    if (speed <= 1e-8) return q;
    float denom = 1.0 + step_dt * grav() * n * n * speed / pow(q.x, 4.0 / 3.0);
    return vec3(q.x, q.yz / denom);
}

void main() {
    // All potential substeps are recorded once. The GPU-computed schedule enables
    // only a prefix, so no CPU readback is required before stepping.
    if (pc.step_index >= control.substeps || dt() <= 0.0) return;

    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    int w = grid_width();
    int hgt = grid_height();
    if (p.x >= w || p.y >= hgt) return;

    int i = idx(p);
    vec4 packed_c = cells_in[i];
    vec3 c = packed_c.xyz;
    float zc = packed_c.w;

    vec4 packed_w = cell_at(p + ivec2(-1, 0));
    vec4 packed_e = cell_at(p + ivec2( 1, 0));
    vec4 packed_s = cell_at(p + ivec2( 0,-1));
    vec4 packed_n = cell_at(p + ivec2( 0, 1));
    vec3 q_w = packed_w.xyz;
    vec3 q_e = packed_e.xyz;
    vec3 q_s = packed_s.xyz;
    vec3 q_n = packed_n.xyz;

    // Closed reflective boundaries for the fixed-domain correctness harness.
    if (p.x == 0) q_w.y = -c.y;
    if (p.x == w - 1) q_e.y = -c.y;
    if (p.y == 0) q_s.z = -c.z;
    if (p.y == hgt - 1) q_n.z = -c.z;

    float zw = p.x > 0 ? packed_w.w : zc;
    float ze = p.x < w - 1 ? packed_e.w : zc;
    float zs = p.y > 0 ? packed_s.w : zc;
    float zn = p.y < hgt - 1 ? packed_n.w : zc;

    HydroInterface fw = interface_x(q_w, zw, c, zc);
    HydroInterface fe = interface_x(c, zc, q_e, ze);
    HydroInterface fs = interface_y(q_s, zs, c, zc);
    HydroInterface fn = interface_y(c, zc, q_n, zn);

    float scale = dt() / dx();
    vec3 updated = c
        - (fe.flux - fw.flux) * scale
        - (fn.flux - fs.flux) * scale;

    // Hydrostatic source correction: exactly cancels the pressure-flux gradient
    // for eta=h+z constant and zero velocity.
    updated.y += 0.5 * grav() * scale
        * (fe.h_left * fe.h_left - fw.h_right * fw.h_right);
    updated.z += 0.5 * grav() * scale
        * (fn.h_left * fn.h_left - fs.h_right * fs.h_right);

    vec4 source = sources[i];
    updated.x = max(updated.x + (max(source.x, 0.0) - max(source.y, 0.0)) * dt(), 0.0);

    if (updated.x <= dry_eps()
            || any(isnan(updated)) || any(isinf(updated))) {
        updated = vec3(0.0);
    } else {
        updated = apply_friction(updated, dt());
    }

    cells_out[i] = vec4(updated, zc);
}
