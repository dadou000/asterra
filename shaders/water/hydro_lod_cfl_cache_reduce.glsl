#[compute]
#version 450

// Reduce cached per-tile CFL/health summaries into the established solver control ABI.
// The dispatch walks a persistent compact live-slot queue rather than atlas capacity.
// mode 0 writes the per-iteration scratch used by CFL preparation.
// mode 1 writes final post-step health diagnostics.

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer TileSummaries {
    uint summary_words[]; // 8 uints per atlas slot
};
layout(set = 0, binding = 1, std430) readonly buffer LiveQueue {
    uint live_words[]; // count, overflow, reserved, reserved, then atlas slot ids
};
layout(set = 0, binding = 2, std430) buffer Control {
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
    vec4 schedule;
} params;

layout(push_constant, std430) uniform ReducePush {
    uint mode; uint pad0; uint pad1; uint pad2;
} pc;

shared uint group_max_speed_bits;
shared uint group_max_depth_bits;
shared uint group_max_cfl_rate_bits;
shared uint group_wet_count;
shared uint group_invalid_count;

void main() {
    if (gl_LocalInvocationIndex == 0u) {
        group_max_speed_bits = 0u;
        group_max_depth_bits = 0u;
        group_max_cfl_rate_bits = 0u;
        group_wet_count = 0u;
        group_invalid_count = 0u;
    }
    barrier();

    uint capacity = max(uint(params.grid_dt.y + 0.5), 1u);
    uint live_count = min(live_words[0], capacity);
    uint live_index = gl_GlobalInvocationID.x;
    if (live_index < live_count) {
        uint slot = live_words[4u + live_index];
        if (slot < capacity) {
            uint base = slot * 8u;
            atomicMax(group_max_speed_bits, summary_words[base + 0u]);
            atomicMax(group_max_depth_bits, summary_words[base + 1u]);
            atomicMax(group_max_cfl_rate_bits, summary_words[base + 2u]);
            atomicAdd(group_wet_count, summary_words[base + 3u]);
            atomicAdd(group_invalid_count, summary_words[base + 4u]);
        } else {
            // A corrupt persistent queue must surface as a solver health failure.
            atomicAdd(group_invalid_count, 1u);
        }
    }

    barrier();
    if (gl_LocalInvocationIndex != 0u) return;
    if (pc.mode == 0u) {
        atomicMax(control.iter_max_speed_bits, group_max_speed_bits);
        atomicMax(control.iter_max_depth_bits, group_max_depth_bits);
        atomicMax(control.iter_max_cfl_rate_bits, group_max_cfl_rate_bits);
        atomicAdd(control.iter_wet_count, group_wet_count);
        atomicAdd(control.iter_invalid_count, group_invalid_count);
    } else {
        atomicMax(control.post_max_speed_bits, group_max_speed_bits);
        atomicMax(control.post_max_depth_bits, group_max_depth_bits);
        atomicAdd(control.post_wet_count, group_wet_count);
        atomicAdd(control.post_invalid_count, group_invalid_count);
    }
}
