#[compute]
#version 450

// One workgroup summarizes one sparse hydrology slot. Each of 64 lanes walks a
// strided subset of the tile, then shared-memory reduction emits compact activity
// and boundary-flux data. The water grid never needs to be read to CPU.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer State {
    vec4 cells[]; // h, hu, hv, bed; slots are contiguous
};
layout(set = 0, binding = 1, std430) readonly buffer Occupancy {
    uint active[];
};
layout(set = 0, binding = 2, std430) writeonly buffer Summaries {
    vec4 summary[]; // 3 vec4 per slot
};
layout(set = 0, binding = 3, std430) readonly buffer Params {
    vec4 grid; // tile_res, capacity, dx, dry_eps
} params;

shared float s_max_depth[64];
shared float s_max_velocity[64];
shared float s_energy[64];
shared float s_west[64];
shared float s_east[64];
shared float s_south[64];
shared float s_north[64];
shared uint s_invalid[64];
shared uint s_wet[64];

bool invalid_state(vec4 v) {
    return any(isnan(v)) || any(isinf(v));
}

void main() {
    uint slot = gl_WorkGroupID.x;
    uint lane = gl_LocalInvocationIndex;
    uint tile_res = max(uint(params.grid.x + 0.5), 1u);
    uint capacity = max(uint(params.grid.y + 0.5), 1u);
    float dx = max(params.grid.z, 1e-4);
    float dry_eps = max(params.grid.w, 1e-8);
    uint tile_cells = tile_res * tile_res;

    float max_depth = 0.0;
    float max_velocity = 0.0;
    float energy = 0.0;
    float west_flux = 0.0;
    float east_flux = 0.0;
    float south_flux = 0.0;
    float north_flux = 0.0;
    uint invalid_count = 0u;
    uint wet_count = 0u;

    bool slot_active = slot < capacity && active[slot] != 0u;
    if (slot_active) {
        uint base = slot * tile_cells;
        for (uint local_i = lane; local_i < tile_cells; local_i += 64u) {
            vec4 q = cells[base + local_i];
            if (invalid_state(q)) {
                invalid_count += 1u;
                continue;
            }
            float h = max(q.x, 0.0);
            max_depth = max(max_depth, h);
            if (h <= dry_eps) continue;

            wet_count += 1u;
            vec2 velocity = q.yz / h;
            float speed = length(velocity);
            max_velocity = max(max_velocity, speed);
            energy += 0.5 * h * dot(velocity, velocity) * dx * dx;

            uint x = local_i % tile_res;
            uint y = local_i / tile_res;
            // hu/hv are discharge per unit width [m^2/s]. Integrating over the
            // boundary face width dx gives m^3/s. Positive values are outward.
            if (x == 0u) west_flux += max(-q.y, 0.0) * dx;
            if (x + 1u == tile_res) east_flux += max(q.y, 0.0) * dx;
            if (y == 0u) south_flux += max(-q.z, 0.0) * dx;
            if (y + 1u == tile_res) north_flux += max(q.z, 0.0) * dx;
        }
    }

    s_max_depth[lane] = max_depth;
    s_max_velocity[lane] = max_velocity;
    s_energy[lane] = energy;
    s_west[lane] = west_flux;
    s_east[lane] = east_flux;
    s_south[lane] = south_flux;
    s_north[lane] = north_flux;
    s_invalid[lane] = invalid_count;
    s_wet[lane] = wet_count;
    barrier();

    for (uint stride = 32u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            s_max_depth[lane] = max(s_max_depth[lane], s_max_depth[lane + stride]);
            s_max_velocity[lane] = max(s_max_velocity[lane], s_max_velocity[lane + stride]);
            s_energy[lane] += s_energy[lane + stride];
            s_west[lane] += s_west[lane + stride];
            s_east[lane] += s_east[lane + stride];
            s_south[lane] += s_south[lane + stride];
            s_north[lane] += s_north[lane + stride];
            s_invalid[lane] += s_invalid[lane + stride];
            s_wet[lane] += s_wet[lane + stride];
        }
        barrier();
    }

    if (lane == 0u && slot < capacity) {
        uint o = slot * 3u;
        if (!slot_active) {
            summary[o + 0u] = vec4(0.0);
            summary[o + 1u] = vec4(0.0);
            summary[o + 2u] = vec4(0.0);
            return;
        }
        summary[o + 0u] = vec4(
            s_max_depth[0], s_max_velocity[0], s_energy[0], float(s_invalid[0]));
        summary[o + 1u] = vec4(
            s_west[0], s_east[0], s_south[0], s_north[0]);
        summary[o + 2u] = vec4(float(s_wet[0]), 1.0, 0.0, 0.0);
    }
}
