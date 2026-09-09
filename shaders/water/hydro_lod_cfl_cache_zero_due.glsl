#[compute]
#version 450

// Clear cached summaries only for tiles present in the current GPU due-slot queue.
// This shader intentionally reuses the SWE indirect command. Only workgroup (0,0)
// for each queued Z lane performs the clear; all other X/Y groups return immediately.

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer DueSlots { uint due_slots[]; };
layout(set = 0, binding = 1, std430) buffer TileSummaries { uint summary_words[]; };
layout(set = 0, binding = 2, std430) readonly buffer Params {
    vec4 grid_dt;
    vec4 physics;
    vec4 schedule;
} params;

void main() {
    if (gl_WorkGroupID.x != 0u || gl_WorkGroupID.y != 0u) return;
    uint due_index = gl_WorkGroupID.z;
    uint slot = due_slots[due_index];
    uint capacity = max(uint(params.grid_dt.y + 0.5), 1u);
    if (slot >= capacity) return;
    uint base = slot * 8u;
    for (uint i = 0u; i < 8u; ++i) summary_words[base + i] = 0u;
}
