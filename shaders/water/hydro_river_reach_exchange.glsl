#[compute]
#version 450

// Operator-split 1D <-> sparse-2D river exchange at promoted river boundaries.
// One invocation owns one sparse member record, so no atomics are required.
//
// meta.y flags:
//   bit 0 = upstream 1D->2D mouth enabled
//   bit 1 = downstream 2D->1D mouth enabled
//
// A one-tile reach enables both bits. A multi-tile cluster enables upstream only
// on its first member and downstream only on its final member; internal mouths are
// connected by the normal sparse SWE interfaces.

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) buffer StateA { vec4 state_a[]; };
layout(set = 0, binding = 1, std430) buffer StateB { vec4 state_b[]; };
layout(set = 0, binding = 2, std430) readonly buffer Occupancy { int occupied[]; };

struct ExchangeRecord {
    uvec4 meta;      // slot, flags, reserved, reserved
    vec4 corridor;   // center_x_cells, center_y_cells, dir_x, dir_y
    vec4 transfer;   // half_width_m, add_volume_m3, exchange_dt_s, mouth_cells
    vec4 velocity;   // add_velocity_u, add_velocity_v, reserved, reserved
};
layout(set = 0, binding = 3, std430) readonly buffer Records {
    ExchangeRecord records[];
};
layout(set = 0, binding = 4, std430) writeonly buffer Results {
    vec4 results[];
};
layout(set = 0, binding = 5, std430) readonly buffer Params {
    uvec4 dims;      // record_count, tile_resolution, capacity, cells_per_tile
    vec4 metric;     // cell_size_m, reserved...
} params;

int index_for(uint slot, int x, int y) {
    return int(slot) * int(params.dims.w) + y * int(params.dims.y) + x;
}

void main() {
    uint ri = gl_GlobalInvocationID.x;
    if (ri >= params.dims.x) return;
    ExchangeRecord rec = records[ri];
    uint slot = rec.meta.x;
    bool upstream_enabled = (rec.meta.y & 1u) != 0u;
    bool downstream_enabled = (rec.meta.y & 2u) != 0u;
    int r = int(params.dims.y);
    if (slot >= params.dims.z || occupied[slot] == 0 || r <= 0) {
        results[ri] = vec4(0.0, 0.0, 0.0, -1.0);
        return;
    }

    vec2 dir = rec.corridor.zw;
    float dir_len = length(dir);
    if (dir_len <= 1e-8) {
        results[ri] = vec4(0.0, 0.0, 0.0, -2.0);
        return;
    }
    dir /= dir_len;
    vec2 normal = vec2(-dir.y, dir.x);
    vec2 center = rec.corridor.xy;
    float dx = max(params.metric.x, 1e-6);
    float half_width = max(rec.transfer.x, 0.5 * dx);
    float dt = max(rec.transfer.z, 0.0);
    float mouth_thickness_cells = clamp(rec.transfer.w, 0.75, 3.0);

    float min_s = 3.402823466e38;
    float max_s = -3.402823466e38;
    uint corridor_count = 0u;
    for (int y = 0; y < r; ++y) {
        for (int x = 0; x < r; ++x) {
            vec2 p = vec2(float(x) + 0.5, float(y) + 0.5);
            vec2 rel = p - center;
            float cross_m = abs(dot(rel, normal)) * dx;
            if (cross_m > half_width) continue;
            float s = dot(rel, dir);
            min_s = min(min_s, s);
            max_s = max(max_s, s);
            corridor_count++;
        }
    }
    if (corridor_count == 0u || min_s > max_s) {
        results[ri] = vec4(0.0, 0.0, 0.0, -3.0);
        return;
    }

    uint upstream_count = 0u;
    float measured_q = 0.0;
    for (int y = 0; y < r; ++y) {
        for (int x = 0; x < r; ++x) {
            vec2 p = vec2(float(x) + 0.5, float(y) + 0.5);
            vec2 rel = p - center;
            float cross_m = abs(dot(rel, normal)) * dx;
            if (cross_m > half_width) continue;
            float s = dot(rel, dir);
            if (upstream_enabled && s <= min_s + mouth_thickness_cells)
                upstream_count++;
            if (downstream_enabled && s >= max_s - mouth_thickness_cells) {
                vec4 q = state_a[index_for(slot, x, y)];
                if (q.x > 0.0) measured_q += max(dot(q.yz, dir), 0.0) * dx;
            }
        }
    }

    float cell_area = dx * dx;
    float requested_add = upstream_enabled ? max(rec.transfer.y, 0.0) : 0.0;
    float add_depth = upstream_count > 0u
        ? requested_add / (float(upstream_count) * cell_area) : 0.0;
    vec2 add_velocity = rec.velocity.xy;
    float actual_add = 0.0;
    float actual_remove = 0.0;

    for (int y = 0; y < r; ++y) {
        for (int x = 0; x < r; ++x) {
            vec2 p = vec2(float(x) + 0.5, float(y) + 0.5);
            vec2 rel = p - center;
            float cross_m = abs(dot(rel, normal)) * dx;
            if (cross_m > half_width) continue;
            float s = dot(rel, dir);
            int idx = index_for(slot, x, y);
            vec4 q = state_a[idx];

            if (upstream_enabled && s <= min_s + mouth_thickness_cells && add_depth > 0.0) {
                q.x += add_depth;
                q.yz += add_depth * add_velocity;
                actual_add += add_depth * cell_area;
            }

            if (downstream_enabled && s >= max_s - mouth_thickness_cells
                    && dt > 0.0 && q.x > 0.0) {
                float qn = max(dot(q.yz, dir), 0.0);
                float requested_remove_h = qn * dt / dx;
                float remove_h = min(max(requested_remove_h, 0.0), q.x);
                if (remove_h > 0.0) {
                    float kept = q.x - remove_h;
                    float keep_fraction = kept / max(q.x, 1e-12);
                    q.yz *= keep_fraction;
                    q.x = kept;
                    actual_remove += remove_h * cell_area;
                }
            }

            state_a[idx] = q;
            state_b[idx] = q;
        }
    }

    // w >= 0 means a valid record. For an upstream-disabled member zero mouth cells
    // are expected and are not an error.
    results[ri] = vec4(actual_add, actual_remove, measured_q,
        upstream_enabled ? float(upstream_count) : 0.0);
}
