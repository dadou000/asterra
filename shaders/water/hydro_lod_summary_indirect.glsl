#[compute]
#version 450

// Derive a one-workgroup-per-tile indirect command from the existing HydroLOD
// due-slot command without exposing the due count to CPU. Source layout is the
// SWE/commit VkDispatchIndirectCommand {tile_groups_x, tile_groups_y, due_count}.
// Destination layout is {1, 1, due_count} for fused CFL/activity summary refresh.

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer SourceDispatchArgs {
    uint source_args[];
};
layout(set = 0, binding = 1, std430) writeonly buffer SummaryDispatchArgs {
    uint summary_args[];
};

void main() {
    summary_args[0] = 1u;
    summary_args[1] = 1u;
    summary_args[2] = source_args[2];
    summary_args[3] = 0u;
}
