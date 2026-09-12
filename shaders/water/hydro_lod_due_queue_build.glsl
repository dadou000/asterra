#[compute]
#version 450

// Compact occupied physical-HydroLOD slots due on the current fine-clock tick.
// The queue remains entirely GPU-resident. args[2] is both the atomic append count
// and VkDispatchIndirectCommand::groupCountZ for the subsequent SWE/commit passes.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer Occupancy {
    int occupied[];
};
layout(set = 0, binding = 1, std430) readonly buffer TileMetadata {
    ivec4 tile_metadata[]; // face, quadtree level, x, y
};
layout(set = 0, binding = 2, std430) readonly buffer Control {
    uint pre_max_speed_bits; uint pre_max_depth_bits; uint pre_wet_count; uint pre_invalid_count;
    uint post_max_speed_bits; uint post_max_depth_bits; uint post_wet_count; uint post_invalid_count;
    float requested_dt; float remaining_dt; float current_dt; float advanced_dt;
    float min_cfl_dt; float last_cfl_dt; uint steps_taken; uint max_substeps;
    uint cfl_clamped; uint iteration_active; uint iter_max_speed_bits; uint iter_max_depth_bits;
    uint iter_wet_count; uint iter_invalid_count; uint reserved0; uint reserved1;
    uint iter_max_cfl_rate_bits; uint reserved2;
    uint temporal_enabled; uint temporal_max_lod; uint temporal_fine_tick; uint temporal_force_sync;
    float temporal_accum0; float temporal_accum1; float temporal_accum2; float temporal_accum3;
    float temporal_accum4; float temporal_accum5; float temporal_accum6; float temporal_accum7;
    float temporal_step0; float temporal_step1; float temporal_step2; float temporal_step3;
    float temporal_step4; float temporal_step5; float temporal_step6; float temporal_step7;
} control;
layout(set = 0, binding = 3, std430) readonly buffer Params {
    vec4 grid_dt;
    vec4 physics;
    vec4 schedule; // max substeps, H0 tile level, HydroLOD enabled, temporal max+1
} params;
layout(set = 0, binding = 4, std430) writeonly buffer DueSlots {
    uint due_slots[];
};
layout(set = 0, binding = 5, std430) buffer DispatchArgs {
    uint args[]; // x_groups, y_groups, due_slot_count, reserved
};

int physical_lod(uint slot) {
    if (params.schedule.z < 0.5) return 0;
    int level = tile_metadata[slot].y;
    if (level < 0) return -1;
    int base_level = int(params.schedule.y + 0.5);
    return max(base_level - level, 0);
}

float scheduled_dt(uint slot) {
    if (control.temporal_enabled == 0u) return max(control.current_dt, 0.0);
    int physical = physical_lod(slot);
    if (physical < 0) return 0.0;
    uint max_lod = min(control.temporal_max_lod, 7u);
    uint lod = uint(clamp(physical, 0, int(max_lod)));
    if (lod == 0u) return control.temporal_step0;
    if (lod == 1u) return control.temporal_step1;
    if (lod == 2u) return control.temporal_step2;
    if (lod == 3u) return control.temporal_step3;
    if (lod == 4u) return control.temporal_step4;
    if (lod == 5u) return control.temporal_step5;
    if (lod == 6u) return control.temporal_step6;
    return control.temporal_step7;
}

void main() {
    if (control.iteration_active == 0u || control.current_dt <= 0.0) return;
    uint slot = gl_GlobalInvocationID.x;
    uint capacity = max(uint(params.grid_dt.y + 0.5), 1u);
    if (slot >= capacity || occupied[slot] == 0 || scheduled_dt(slot) <= 0.0) return;

    uint queue_index = atomicAdd(args[2], 1u);
    if (queue_index < capacity) due_slots[queue_index] = slot;
}
