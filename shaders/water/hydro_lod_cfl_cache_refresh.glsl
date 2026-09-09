#[compute]
#version 450

// Fused per-tile summary refresh from canonical atlas A.
// mode 0: full topology rebuild, workgroup Z is the atlas slot.
// mode 1: due-only refresh, workgroup Z indexes the GPU due-slot queue.
//
// One 64-thread workgroup scans one tile and produces both persistent products:
//   - 32-byte CFL/health summary used by the fine-clock timestep reducer;
//   - the established 80-byte HydroTileActivityGPU summary consumed by CPU policy
//     and HydroFrontierCandidatesGPU.
// This removes the separate post-solve full-cell activity classification pass.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer State { vec4 cells[]; };
layout(set = 0, binding = 1, std430) readonly buffer Occupancy { int occupied[]; };
layout(set = 0, binding = 2, std430) readonly buffer TileMetadata { ivec4 tile_metadata[]; };
layout(set = 0, binding = 3, std430) readonly buffer Params {
    vec4 grid_dt;
    vec4 physics;
    vec4 schedule;
} params;
layout(set = 0, binding = 4, std430) readonly buffer DueSlots { uint due_slots[]; };
layout(set = 0, binding = 5, std430) writeonly buffer TileSummaries {
    uint summary_words[]; // 8 uints per slot
};
layout(set = 0, binding = 6, std430) writeonly buffer ActivitySummaries {
    vec4 activity_summary[]; // exact HydroTileActivityGPU 5-vec4 ABI
};

layout(push_constant, std430) uniform RefreshPush {
    uint mode; uint pad0; uint pad1; uint pad2;
} pc;

const float NEG_FLT_MAX = -3.402823e38;

shared float s_max_characteristic[64];
shared float s_max_depth[64];
shared float s_max_velocity[64];
shared float s_energy[64];
shared float s_max_cfl_rate[64];
shared float s_west[64];
shared float s_east[64];
shared float s_south[64];
shared float s_north[64];
shared float s_wet_west[64];
shared float s_wet_east[64];
shared float s_wet_south[64];
shared float s_wet_north[64];
shared float s_eta_west[64];
shared float s_eta_east[64];
shared float s_eta_south[64];
shared float s_eta_north[64];
shared uint s_wet[64];
shared uint s_invalid[64];

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

float predicted_dry_flux_per_width(float h, float outward_momentum, float gravity) {
    if (h <= max(params.physics.y, 1e-8)) return 0.0;
    float u = outward_momentum / h;
    float a = abs(u) + sqrt(max(gravity, 1e-4) * h);
    return max(0.5 * (outward_momentum + a * h), 0.0);
}

void write_empty(uint slot) {
    uint cfl_base = slot * 8u;
    for (uint i = 0u; i < 8u; ++i) summary_words[cfl_base + i] = 0u;
    uint a = slot * 5u;
    activity_summary[a + 0u] = vec4(0.0);
    activity_summary[a + 1u] = vec4(0.0);
    activity_summary[a + 2u] = vec4(0.0);
    activity_summary[a + 3u] = vec4(0.0);
    activity_summary[a + 4u] = vec4(NEG_FLT_MAX);
}

void main() {
    uint lane = gl_LocalInvocationIndex;
    uint slot = pc.mode == 0u ? gl_WorkGroupID.z : due_slots[gl_WorkGroupID.z];
    uint cap = uint(capacity());
    if (slot >= cap) return;

    bool slot_live = occupied[slot] != 0;
    if (!slot_live) {
        if (lane == 0u && pc.mode == 0u) write_empty(slot);
        return;
    }

    int lod = physical_lod(slot);
    if (lod < 0) {
        if (lane == 0u) {
            uint cfl_base = slot * 8u;
            summary_words[cfl_base + 0u] = 0u;
            summary_words[cfl_base + 1u] = 0u;
            summary_words[cfl_base + 2u] = 0u;
            summary_words[cfl_base + 3u] = 0u;
            summary_words[cfl_base + 4u] = 1u;
            summary_words[cfl_base + 5u] = 0u;
            summary_words[cfl_base + 6u] = 0u;
            summary_words[cfl_base + 7u] = 0u;
            uint a = slot * 5u;
            activity_summary[a + 0u] = vec4(0.0, 0.0, 0.0, 1.0);
            activity_summary[a + 1u] = vec4(0.0);
            activity_summary[a + 2u] = vec4(0.0, 1.0, 0.0, 0.0);
            activity_summary[a + 3u] = vec4(0.0);
            activity_summary[a + 4u] = vec4(NEG_FLT_MAX);
        }
        return;
    }

    int r = tile_res();
    uint tile_cells = uint(cells_per_tile());
    float dry_eps = max(params.physics.y, 1e-8);
    float gravity = max(params.physics.x, 1e-4);
    float dx = slot_dx(lod);

    float max_characteristic = 0.0;
    float max_depth = 0.0;
    float max_velocity = 0.0;
    float energy = 0.0;
    float max_cfl_rate = 0.0;
    float west_flux = 0.0;
    float east_flux = 0.0;
    float south_flux = 0.0;
    float north_flux = 0.0;
    float wet_west = 0.0;
    float wet_east = 0.0;
    float wet_south = 0.0;
    float wet_north = 0.0;
    float eta_west = NEG_FLT_MAX;
    float eta_east = NEG_FLT_MAX;
    float eta_south = NEG_FLT_MAX;
    float eta_north = NEG_FLT_MAX;
    uint wet_count = 0u;
    uint invalid_count = 0u;

    uint base_cell = slot * tile_cells;
    int max_temporal_lod = temporal_max_lod();
    for (uint local_i = lane; local_i < tile_cells; local_i += 64u) {
        vec4 q = cells[base_cell + local_i];
        bool bad = invalid_state(q);
        if (bad) {
            invalid_count += 1u;
            continue;
        }

        float h = max(q.x, 0.0);
        max_depth = max(max_depth, h);
        if (h <= dry_eps) continue;

        wet_count += 1u;
        vec2 velocity = q.yz / h;
        float velocity_mag = length(velocity);
        float c = sqrt(gravity * h);
        float characteristic = max(abs(velocity.x) + c, abs(velocity.y) + c);
        float cfl_rate = characteristic / dx;
        if (max_temporal_lod >= 0)
            cfl_rate *= exp2(float(clamp(lod, 0, max_temporal_lod)));
        if (isnan(velocity_mag) || isinf(velocity_mag)
                || isnan(characteristic) || isinf(characteristic)
                || isnan(cfl_rate) || isinf(cfl_rate)) {
            invalid_count += 1u;
            continue;
        }

        max_velocity = max(max_velocity, velocity_mag);
        max_characteristic = max(max_characteristic, characteristic);
        max_cfl_rate = max(max_cfl_rate, cfl_rate);
        energy += 0.5 * h * dot(velocity, velocity) * dx * dx;
        float eta = q.w + h;

        uint x = local_i % uint(r);
        uint y = local_i / uint(r);
        if (x == 0u) {
            float m = -q.y;
            west_flux += max(m, 0.0) * dx;
            wet_west += predicted_dry_flux_per_width(h, m, gravity) * dx;
            eta_west = max(eta_west, eta);
        }
        if (x + 1u == uint(r)) {
            float m = q.y;
            east_flux += max(m, 0.0) * dx;
            wet_east += predicted_dry_flux_per_width(h, m, gravity) * dx;
            eta_east = max(eta_east, eta);
        }
        if (y == 0u) {
            float m = -q.z;
            south_flux += max(m, 0.0) * dx;
            wet_south += predicted_dry_flux_per_width(h, m, gravity) * dx;
            eta_south = max(eta_south, eta);
        }
        if (y + 1u == uint(r)) {
            float m = q.z;
            north_flux += max(m, 0.0) * dx;
            wet_north += predicted_dry_flux_per_width(h, m, gravity) * dx;
            eta_north = max(eta_north, eta);
        }
    }

    s_max_characteristic[lane] = max_characteristic;
    s_max_depth[lane] = max_depth;
    s_max_velocity[lane] = max_velocity;
    s_energy[lane] = energy;
    s_max_cfl_rate[lane] = max_cfl_rate;
    s_west[lane] = west_flux;
    s_east[lane] = east_flux;
    s_south[lane] = south_flux;
    s_north[lane] = north_flux;
    s_wet_west[lane] = wet_west;
    s_wet_east[lane] = wet_east;
    s_wet_south[lane] = wet_south;
    s_wet_north[lane] = wet_north;
    s_eta_west[lane] = eta_west;
    s_eta_east[lane] = eta_east;
    s_eta_south[lane] = eta_south;
    s_eta_north[lane] = eta_north;
    s_wet[lane] = wet_count;
    s_invalid[lane] = invalid_count;
    barrier();

    for (uint stride = 32u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            s_max_characteristic[lane] = max(
                s_max_characteristic[lane], s_max_characteristic[lane + stride]);
            s_max_depth[lane] = max(s_max_depth[lane], s_max_depth[lane + stride]);
            s_max_velocity[lane] = max(s_max_velocity[lane], s_max_velocity[lane + stride]);
            s_energy[lane] += s_energy[lane + stride];
            s_max_cfl_rate[lane] = max(s_max_cfl_rate[lane], s_max_cfl_rate[lane + stride]);
            s_west[lane] += s_west[lane + stride];
            s_east[lane] += s_east[lane + stride];
            s_south[lane] += s_south[lane + stride];
            s_north[lane] += s_north[lane + stride];
            s_wet_west[lane] += s_wet_west[lane + stride];
            s_wet_east[lane] += s_wet_east[lane + stride];
            s_wet_south[lane] += s_wet_south[lane + stride];
            s_wet_north[lane] += s_wet_north[lane + stride];
            s_eta_west[lane] = max(s_eta_west[lane], s_eta_west[lane + stride]);
            s_eta_east[lane] = max(s_eta_east[lane], s_eta_east[lane + stride]);
            s_eta_south[lane] = max(s_eta_south[lane], s_eta_south[lane + stride]);
            s_eta_north[lane] = max(s_eta_north[lane], s_eta_north[lane + stride]);
            s_wet[lane] += s_wet[lane + stride];
            s_invalid[lane] += s_invalid[lane + stride];
        }
        barrier();
    }

    if (lane != 0u) return;

    uint cfl_base = slot * 8u;
    summary_words[cfl_base + 0u] = floatBitsToUint(max(s_max_characteristic[0], 0.0));
    summary_words[cfl_base + 1u] = floatBitsToUint(max(s_max_depth[0], 0.0));
    summary_words[cfl_base + 2u] = floatBitsToUint(max(s_max_cfl_rate[0], 0.0));
    summary_words[cfl_base + 3u] = s_wet[0];
    summary_words[cfl_base + 4u] = s_invalid[0];
    summary_words[cfl_base + 5u] = 0u;
    summary_words[cfl_base + 6u] = 0u;
    summary_words[cfl_base + 7u] = 0u;

    uint a = slot * 5u;
    activity_summary[a + 0u] = vec4(
        s_max_depth[0], s_max_velocity[0], s_energy[0], float(s_invalid[0]));
    activity_summary[a + 1u] = vec4(
        s_west[0], s_east[0], s_south[0], s_north[0]);
    activity_summary[a + 2u] = vec4(float(s_wet[0]), 1.0, 0.0, 0.0);
    activity_summary[a + 3u] = vec4(
        s_wet_west[0], s_wet_east[0], s_wet_south[0], s_wet_north[0]);
    activity_summary[a + 4u] = vec4(
        s_eta_west[0], s_eta_east[0], s_eta_south[0], s_eta_north[0]);
}
