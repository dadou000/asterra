#[compute]
#version 450

// Convert the persistent live-slot count to VkDispatchIndirectCommand form for
// CFL/health reduction. One 256-thread group handles 256 cached tile summaries.

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer LiveQueue {
    uint words[]; // count, overflow, reserved, reserved, then slot ids
};
layout(set = 0, binding = 1, std430) buffer DispatchArgs {
    uint args[]; // groupCountX, groupCountY, groupCountZ, reserved
};
layout(set = 0, binding = 2, std430) readonly buffer Params {
    vec4 grid_dt;
    vec4 physics;
    vec4 schedule;
} params;

void main() {
    uint capacity = max(uint(params.grid_dt.y + 0.5), 1u);
    uint count = min(words[0], capacity);
    // Keep one harmless group when empty so backend validation never depends on
    // whether zero-sized indirect compute dispatches are accepted.
    args[0] = max((count + 255u) / 256u, 1u);
    args[1] = 1u;
    args[2] = 1u;
    args[3] = 0u;
}
