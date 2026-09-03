#[compute]
#version 450

// Refresh per-tile CFL/health summaries from canonical atlas A.
// mode 0: full topology rebuild, workgroup Z is the atlas slot.
// mode 1: due-only refresh, workgroup Z indexes the GPU due-slot queue.
//
// X/Y workgroups cover 8x8 cell blocks. One representative invocation per block
// atomically contributes its local reduction into the owning tile summary.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer State { vec4 cells[]; };
layout(set = 0, binding = 1, std430) readonly buffer Occupancy { int occupied[]; };
layout(set = 0, binding = 2, std430) readonly buffer TileMetadata { ivec4 tile_metadata[]; };
layout(set = 0, binding = 3, std430) readonly buffer Params {
    vec4 grid_dt;
    vec4 physics;
    vec4 schedule;
} params;
layout(set = 0, binding = 4, std430) readonly buffer DueSlots { uint due_slots[]; };
layout(set = 0, binding = 5, std430) buffer TileSummaries {
    uint summary_words[]; // 8 uints per slot
};

layout(push_constant, std430) uniform RefreshPush {
    uint mode; uint pad0; uint pad1; uint pad2;
} pc;

shared uint group_max_speed_bits;
shared uint group_max_depth_bits;
shared uint group_max_cfl_rate_bits;
shared uint group_wet_count;
shared uint group_invalid_count;

int tile_res() { return max(int(params.grid_dt.x + 0.5), 1); }
int capacity() { return max(int(params.grid_dt.y + 0.5), 1); }
int cells_per_tile() { int r = tile_res(); return r * r; }

int physical_lod(uint slot) {
    if (params.schedule.z < 0.5) return 0;
    int base_level = int(params.schedule.y + 0.5);
    int level = tile_metadata[slot].y;
    if (level < 0 || level > base_level) return -1;
    return base_level - level;
}

float slot_dx(int lod) {
    return max(params.grid_dt.z, 1e-4) * exp2(float(max(lod, 0)));
}

int temporal_max_lod() {
    int encoded = int(params.schedule.w + 0.5);
    return encoded <= 0 ? -1 : encoded - 1;
}

bool invalid_state(vec4 s) {
    return any(isnan(s)) || any(isinf(s)) || s.x < -max(params.physics.y, 1e-8);
}

void main() {
    if (gl_LocalInvocationIndex == 0u) {
        group_max_speed_bits = 0u;
        group_max_depth_bits = 0u;
        group_max_cfl_rate_bits = 0u;
        group_wet_count = 0u;
        group_invalid_count = 0u;
    }
    barrier();

    uint slot = pc.mode == 0u ? gl_WorkGroupID.z : due_slots[gl_WorkGroupID.z];
    int r = tile_res();
    bool slot_live = slot < uint(capacity()) && occupied[slot] != 0;
    int lod = slot_live ? physical_lod(slot) : -1;

    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    bool cell_live = slot_live && lod >= 0 && x < uint(r) && y < uint(r);
    if (cell_live) {
        int i = int(slot) * cells_per_tile() + int(y) * r + int(x);
        vec4 s = cells[i];
        bool bad = invalid_state(s);
        float depth = bad ? 0.0 : max(s.x, 0.0);
        bool wet = depth > max(params.physics.y, 1e-8);
        float speed = 0.0;
        float cfl_rate = 0.0;
        if (wet) {
            vec2 velocity = s.yz / max(depth, 1e-8);
            float c = sqrt(max(params.physics.x, 1e-4) * depth);
            speed = max(abs(velocity.x) + c, abs(velocity.y) + c);
            cfl_rate = speed / slot_dx(lod);
            int max_lod = temporal_max_lod();
            if (max_lod >= 0)
                cfl_rate *= exp2(float(clamp(lod, 0, max_lod)));
            if (isnan(speed) || isinf(speed) || isnan(cfl_rate) || isinf(cfl_rate)) {
                speed = 0.0;
                cfl_rate = 0.0;
                bad = true;
            }
        }
        atomicMax(group_max_speed_bits, floatBitsToUint(max(speed, 0.0)));
        atomicMax(group_max_depth_bits, floatBitsToUint(max(depth, 0.0)));
        atomicMax(group_max_cfl_rate_bits, floatBitsToUint(max(cfl_rate, 0.0)));
        if (wet) atomicAdd(group_wet_count, 1u);
        if (bad) atomicAdd(group_invalid_count, 1u);
    } else if (slot_live && lod < 0 && gl_LocalInvocationIndex == 0u
            && gl_WorkGroupID.x == 0u && gl_WorkGroupID.y == 0u) {
        // Published metadata outside the configured hierarchy is a health error.
        atomicAdd(group_invalid_count, 1u);
    }

    barrier();
    if (gl_LocalInvocationIndex != 0u || !slot_live) return;
    uint base = slot * 8u;
    atomicMax(summary_words[base + 0u], group_max_speed_bits);
    atomicMax(summary_words[base + 1u], group_max_depth_bits);
    atomicMax(summary_words[base + 2u], group_max_cfl_rate_bits);
    atomicAdd(summary_words[base + 3u], group_wet_count);
    atomicAdd(summary_words[base + 4u], group_invalid_count);
}
