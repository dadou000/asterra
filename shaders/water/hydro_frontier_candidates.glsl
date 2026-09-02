#[compute]
#version 450

// Converts compact per-tile activity summaries into a still-smaller frontier
// queue. Terrain reachability and cube-face neighbor mapping are intentionally not
// guessed here; each entry is only a source-slot + boundary-direction + flux
// candidate for the policy/topology layer.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer Summaries {
    vec4 summary[]; // 3 vec4 per slot
};
layout(set = 0, binding = 1, std430) buffer Queue {
    uint count;
    uint overflow;
    uint reserved0;
    uint reserved1;
    uvec4 entries[]; // slot, direction, floatBitsToUint(flux), reserved
} queue_out;
layout(set = 0, binding = 2, std430) readonly buffer Params {
    vec4 config; // capacity, threshold_m3s, max_candidates, reserved
} params;

void emit_candidate(uint slot, uint direction, float flux, uint max_candidates) {
    uint index = atomicAdd(queue_out.count, 1u);
    if (index >= max_candidates) {
        atomicOr(queue_out.overflow, 1u);
        return;
    }
    queue_out.entries[index] = uvec4(slot, direction, floatBitsToUint(flux), 0u);
}

void main() {
    uint slot = gl_GlobalInvocationID.x;
    uint capacity = max(uint(params.config.x + 0.5), 1u);
    uint max_candidates = max(uint(params.config.z + 0.5), 1u);
    if (slot >= capacity) return;

    uint o = slot * 3u;
    vec4 health = summary[o + 0u];
    vec4 edges = summary[o + 1u];
    vec4 ownership = summary[o + 2u];
    if (ownership.y < 0.5 || health.w > 0.5) return;

    float threshold = max(params.config.y, 0.0);
    if (edges.x > threshold) emit_candidate(slot, 0u, edges.x, max_candidates); // west
    if (edges.y > threshold) emit_candidate(slot, 1u, edges.y, max_candidates); // east
    if (edges.z > threshold) emit_candidate(slot, 2u, edges.z, max_candidates); // south
    if (edges.w > threshold) emit_candidate(slot, 3u, edges.w, max_candidates); // north
}
