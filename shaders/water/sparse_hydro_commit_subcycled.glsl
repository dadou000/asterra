#[compute]
#version 450

// Canonicalize only the compact GPU due-slot queue produced for this fine-clock
// tick. The dispatch footprint matches the 8x8 SWE kernel, so the same
// VkDispatchIndirectCommand drives both passes. Non-due atlas slots launch no
// commit work at all and canonical A remains untouched.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer ScratchState {
    vec4 scratch_state[];
};
layout(set = 0, binding = 1, std430) writeonly buffer CanonicalState {
    vec4 canonical_state[];
};
layout(set = 0, binding = 2, std430) readonly buffer CommitParams {
    uvec4 config; // total_cells, cells_per_tile, capacity, reserved
} commit_params;
layout(set = 0, binding = 3, std430) readonly buffer DueSlots {
    uint due_slots[];
};

void main() {
    uint queue_index = gl_GlobalInvocationID.z;
    if (queue_index >= commit_params.config.z) return;
    uint slot = due_slots[queue_index];
    if (slot >= commit_params.config.z) return;

    uint cells_per_tile = max(commit_params.config.y, 1u);
    uint tile_res = max(uint(sqrt(float(cells_per_tile)) + 0.5), 1u);
    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    if (x >= tile_res || y >= tile_res) return;

    uint local_i = y * tile_res + x;
    if (local_i >= cells_per_tile) return;
    uint i = slot * cells_per_tile + local_i;
    if (i >= commit_params.config.x) return;
    canonical_state[i] = scratch_state[i];
}
