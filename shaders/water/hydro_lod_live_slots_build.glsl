#[compute]
#version 450

// Rebuild the persistent occupied-slot queue after a sparse topology revision.
// This pass is intentionally topology-scoped rather than fine-tick scoped.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer Occupancy {
    int occupied[];
};
layout(set = 0, binding = 1, std430) buffer LiveQueue {
    uint words[]; // count, overflow, reserved, reserved, then slot ids
};
layout(set = 0, binding = 2, std430) readonly buffer Params {
    vec4 grid_dt;  // tile_res, capacity, H0 dx, requested dt
    vec4 physics;
    vec4 schedule;
} params;

void main() {
    uint slot = gl_GlobalInvocationID.x;
    uint capacity = max(uint(params.grid_dt.y + 0.5), 1u);
    if (slot >= capacity || occupied[slot] == 0) return;

    uint index = atomicAdd(words[0], 1u);
    if (index < capacity) {
        words[4u + index] = slot;
    } else {
        atomicOr(words[1], 1u);
    }
}
