#[compute]
#version 450

// Conservative 2:1 physical-HydroLOD prolongation.
// Each parent cell maps to four fine cells across the child quartet. Child beds are
// already staged from terrain. A local water-filling solve finds one free surface
// whose four fine depths sum to exactly 4*h_parent, which conserves volume because
// child cell area is parent area / 4. All wet children inherit parent velocity, so
// integrated hu/hv are conserved by the same identity.
//
// Children are hidden (occupancy=0); parent remains authoritative during the pass.
// Both child ping-pong states are written identically and stale source layers are
// cleared before publication.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer StateIn {
    vec4 cells_in[];
};
layout(set = 0, binding = 1, std430) buffer StateA {
    vec4 cells_a[];
};
layout(set = 0, binding = 2, std430) buffer StateB {
    vec4 cells_b[];
};
layout(set = 0, binding = 3, std430) buffer GameplaySources {
    vec4 gameplay_sources[];
};
layout(set = 0, binding = 4, std430) buffer AtmosphericSources {
    vec4 atmospheric_sources[];
};
layout(set = 0, binding = 5, std430) readonly buffer Occupancy {
    int occupied[];
};
layout(set = 0, binding = 6, std430) readonly buffer Params {
    uvec4 slots0; // parent, child SW(0), child SE(1), child NW(2)
    uvec4 slots1; // child NE(3), tile_res, capacity, reserved
} params;

uint tile_res() { return max(params.slots1.y, 1u); }
uint cells_per_tile() { uint r = tile_res(); return r * r; }
uint idx(uint slot, uvec2 p) { return slot * cells_per_tile() + p.y * tile_res() + p.x; }

uint child_slot(uint q) {
    if (q == 0u) return params.slots0.y;
    if (q == 1u) return params.slots0.z;
    if (q == 2u) return params.slots0.w;
    return params.slots1.x;
}

void swap_if_greater(inout float a, inout float b) {
    if (a > b) { float t = a; a = b; b = t; }
}

float water_level(float target_sum_depth, vec4 bed) {
    float a = bed.x, b = bed.y, c = bed.z, d = bed.w;
    swap_if_greater(a, b); swap_if_greater(c, d);
    swap_if_greater(a, c); swap_if_greater(b, d);
    swap_if_greater(b, c);

    float eta = a + target_sum_depth;
    if (eta <= b) return eta;
    eta = (target_sum_depth + a + b) * 0.5;
    if (eta <= c) return eta;
    eta = (target_sum_depth + a + b + c) / 3.0;
    if (eta <= d) return eta;
    return (target_sum_depth + a + b + c + d) * 0.25;
}

void fine_address(uvec2 parent_cell, uint sub, out uint slot, out uvec2 local) {
    uint r = tile_res();
    uint sx = sub & 1u;
    uint sy = (sub >> 1u) & 1u;
    uvec2 g = parent_cell * 2u + uvec2(sx, sy);
    uint cx = g.x >= r ? 1u : 0u;
    uint cy = g.y >= r ? 1u : 0u;
    uint q = cx | (cy << 1u);
    slot = child_slot(q);
    local = uvec2(g.x % r, g.y % r);
}

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    uint r = tile_res();
    uint parent_slot = params.slots0.x;
    uint cap = max(params.slots1.z, 1u);
    if (p.x >= r || p.y >= r || parent_slot >= cap || occupied[parent_slot] == 0) return;
    for (uint q = 0u; q < 4u; ++q) {
        uint s = child_slot(q);
        if (s >= cap || occupied[s] != 0) return;
    }

    vec4 parent = cells_in[idx(parent_slot, p)];
    if (any(isnan(parent)) || any(isinf(parent))) return;
    float hp = max(parent.x, 0.0);
    vec2 velocity = hp > 1e-8 ? parent.yz / hp : vec2(0.0);

    uint slots[4];
    uvec2 locals[4];
    vec4 beds;
    for (uint sub = 0u; sub < 4u; ++sub) {
        fine_address(p, sub, slots[sub], locals[sub]);
        vec4 staged = cells_a[idx(slots[sub], locals[sub])];
        if (any(isnan(staged)) || any(isinf(staged))) return;
        beds[sub] = staged.w;
    }

    float target_sum = 4.0 * hp;
    float eta = water_level(target_sum, beds);
    vec4 h = max(vec4(eta) - beds, vec4(0.0));

    // One FP32 residual correction closes the conservative sum after max/rounding.
    float residual = target_sum - (h.x + h.y + h.z + h.w);
    uint deepest = 0u;
    if (beds.y < beds[deepest]) deepest = 1u;
    if (beds.z < beds[deepest]) deepest = 2u;
    if (beds.w < beds[deepest]) deepest = 3u;
    h[deepest] = max(h[deepest] + residual, 0.0);

    for (uint sub = 0u; sub < 4u; ++sub) {
        uint i = idx(slots[sub], locals[sub]);
        vec4 state = vec4(h[sub], h[sub] * velocity.x,
            h[sub] * velocity.y, beds[sub]);
        cells_a[i] = state;
        cells_b[i] = state;
        gameplay_sources[i] = vec4(0.0);
        atmospheric_sources[i] = vec4(0.0);
    }
}
