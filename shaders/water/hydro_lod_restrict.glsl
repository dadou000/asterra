#[compute]
#version 450

// Conservative 2:1 physical-HydroLOD restriction.
// Four N x N child tiles cover one N x N parent tile. Each parent cell owns a 2x2
// fine-cell block, so area-weighted restriction is exactly the arithmetic mean of
// h, hu, hv and bed. Averaging bed as well preserves a lake-at-rest free surface
// when the four fine cells share eta, while integrated mass and momentum are exact
// up to FP32 arithmetic.
//
// Destination parent is hidden (occupancy=0) during this pass. Both ping-pong states
// are written identically and source layers for the recycled parent slot are cleared.

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

vec4 fine_state(uvec2 global_fine) {
    uint r = tile_res();
    uint cx = global_fine.x >= r ? 1u : 0u;
    uint cy = global_fine.y >= r ? 1u : 0u;
    uint q = cx | (cy << 1u);
    uvec2 local = uvec2(global_fine.x % r, global_fine.y % r);
    return cells_in[idx(child_slot(q), local)];
}

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    uint r = tile_res();
    uint parent = params.slots0.x;
    uint cap = max(params.slots1.z, 1u);
    if (p.x >= r || p.y >= r || parent >= cap) return;
    if (occupied[parent] != 0) return;
    for (uint q = 0u; q < 4u; ++q) {
        uint s = child_slot(q);
        if (s >= cap || occupied[s] == 0) return;
    }

    uvec2 g = p * 2u;
    vec4 s00 = fine_state(g + uvec2(0u, 0u));
    vec4 s10 = fine_state(g + uvec2(1u, 0u));
    vec4 s01 = fine_state(g + uvec2(0u, 1u));
    vec4 s11 = fine_state(g + uvec2(1u, 1u));
    if (any(isnan(s00)) || any(isinf(s00)) || any(isnan(s10)) || any(isinf(s10))
            || any(isnan(s01)) || any(isinf(s01)) || any(isnan(s11)) || any(isinf(s11)))
        return;

    vec4 parent_state = 0.25 * (s00 + s10 + s01 + s11);
    parent_state.x = max(parent_state.x, 0.0);
    uint i = idx(parent, p);
    cells_a[i] = parent_state;
    cells_b[i] = parent_state;
    gameplay_sources[i] = vec4(0.0);
    atmospheric_sources[i] = vec4(0.0);
}
