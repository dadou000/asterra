#[compute]
#version 450

// Copy the completed sparse substep back into the canonical atlas state A.
// Keeping A authoritative avoids forcing activity/frontier/reconstruction passes
// to track ping-pong parity during the first sparse-solver integration stage.

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer ScratchState {
    vec4 scratch_state[];
};
layout(set = 0, binding = 1, std430) writeonly buffer CanonicalState {
    vec4 canonical_state[];
};
layout(set = 0, binding = 2, std430) readonly buffer Occupancy {
    int occupied[];
};
layout(set = 0, binding = 3, std430) readonly buffer Params {
    uvec4 config; // total_cells, cells_per_tile, capacity, reserved
} params;

void main() {
    uint i = gl_GlobalInvocationID.x;
    if (i >= params.config.x) return;
    uint cells_per_tile = max(params.config.y, 1u);
    uint slot = i / cells_per_tile;
    if (slot >= params.config.z) return;
    canonical_state[i] = occupied[slot] != 0 ? scratch_state[i] : vec4(0.0);
}
