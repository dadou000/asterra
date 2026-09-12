#[compute]
#version 450

// Convert the compact activity count into VkDispatchIndirectCommand form for
// downstream active-slot consumers such as frontier generation. Keep one no-op
// group when the queue is empty so drivers never see a zero-sized indirect launch;
// the frontier shader immediately rejects compact_index >= active_count.

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer CompactQueue {
    uint words[]; // words[0] = active entry count
};
layout(set = 0, binding = 1, std430) buffer DispatchArgs {
    uint args[]; // groupCountX, groupCountY, groupCountZ, reserved
};
layout(set = 0, binding = 2, std430) readonly buffer Params {
    uvec4 config; // capacity, max_entries, reserved, reserved
} params;

void main() {
    uint count = min(words[0], max(params.config.y, 1u));
    args[0] = max((count + 63u) / 64u, 1u);
    args[1] = 1u;
    args[2] = 1u;
    args[3] = 0u;
}
