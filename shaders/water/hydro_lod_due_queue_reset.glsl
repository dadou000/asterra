#[compute]
#version 450

// Reset the GPU-resident due-slot queue's VkDispatchIndirectCommand before one
// fine-clock tick. X/Y are the per-tile 8x8 workgroup footprint shared by the
// subcycled SWE and canonical commit kernels; Z is atomically filled with the
// number of occupied physical-LOD slots due on this tick.

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) buffer DispatchArgs {
    uint args[]; // x_groups, y_groups, due_slot_count, reserved
};
layout(set = 0, binding = 1, std430) readonly buffer Params {
    vec4 grid_dt;
    vec4 physics;
    vec4 schedule;
} params;

void main() {
    uint tile_res = max(uint(params.grid_dt.x + 0.5), 1u);
    args[0] = (tile_res + 7u) / 8u;
    args[1] = (tile_res + 7u) / 8u;
    args[2] = 0u;
    args[3] = 0u;
}
