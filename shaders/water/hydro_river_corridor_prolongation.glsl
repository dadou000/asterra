#[compute]
#version 450

// Terrain-aware 1D river reach -> sparse 2D corridor prolongation for one hidden tile.
//
// The destination terrain has already been staged dry into A+B. Only cells inside
// a narrow line corridor participate. A single invocation solves one level free
// surface over those corridor beds such that the conserved target parcel volume is
// represented exactly as closely as FP32 permits. Cells outside the corridor stay
// dry. A+B receive identical h/hu/hv so atlas A remains canonical after publish.
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) buffer StateA {
    vec4 state_a[];
};
layout(set = 0, binding = 1, std430) buffer StateB {
    vec4 state_b[];
};
layout(set = 0, binding = 2, std430) readonly buffer Params {
    uvec4 config;   // slot, cells_per_tile, tile_resolution, capacity
    vec4 parcel;    // target_volume_m3, cell_area_m2, velocity_u, velocity_v
    vec4 corridor;  // center_x_cells, center_y_cells, dir_x, dir_y
    vec4 geometry;  // half_width_cells, bisection_iterations, max_cells, reserved
} params;

bool corridor_cell(uint index, uint resolution, vec2 center, vec2 dir, float half_width) {
    uint x = index % resolution;
    uint y = index / resolution;
    vec2 p = vec2(float(x) + 0.5, float(y) + 0.5);
    vec2 rel = p - center;
    float perpendicular = abs(rel.x * dir.y - rel.y * dir.x);
    return perpendicular <= half_width;
}

void main() {
    uint slot = params.config.x;
    uint count = params.config.y;
    uint resolution = params.config.z;
    uint capacity = params.config.w;
    if (slot >= capacity || count == 0u || resolution == 0u ||
            count != resolution * resolution || count > uint(params.geometry.z + 0.5)) return;

    float target_volume = max(params.parcel.x, 0.0);
    float cell_area = max(params.parcel.y, 1e-12);
    if (target_volume <= 0.0) return;

    vec2 dir = params.corridor.zw;
    float dir_len = length(dir);
    if (!(dir_len > 1e-8)) return;
    dir /= dir_len;
    vec2 center = params.corridor.xy;
    float half_width = max(params.geometry.x, 0.51);

    uint base = slot * count;
    float min_bed = 3.402823466e38;
    float max_bed = -3.402823466e38;
    uint deepest = 0u;
    uint eligible = 0u;
    for (uint i = 0u; i < count; ++i) {
        if (!corridor_cell(i, resolution, center, dir, half_width)) continue;
        float bed = state_a[base + i].w;
        if (isnan(bed) || isinf(bed)) return;
        ++eligible;
        if (bed < min_bed) {
            min_bed = bed;
            deepest = i;
        }
        max_bed = max(max_bed, bed);
    }
    if (eligible == 0u) return;

    float target_depth_sum = target_volume / cell_area;
    float relief = max(max_bed - min_bed, 0.0);
    float low = 0.0;
    float high = max(relief + target_depth_sum, 1e-9);
    uint iterations = clamp(uint(params.geometry.y + 0.5), 8u, 40u);

    for (uint iter = 0u; iter < iterations; ++iter) {
        float mid = 0.5 * (low + high);
        float depth_sum = 0.0;
        for (uint i = 0u; i < count; ++i) {
            if (!corridor_cell(i, resolution, center, dir, half_width)) continue;
            float rel_bed = max(state_a[base + i].w - min_bed, 0.0);
            depth_sum += max(mid - rel_bed, 0.0);
        }
        if (depth_sum < target_depth_sum) low = mid;
        else high = mid;
    }

    float eta_rel = high;
    float depth_sum = 0.0;
    for (uint i = 0u; i < count; ++i) {
        if (!corridor_cell(i, resolution, center, dir, half_width)) continue;
        float rel_bed = max(state_a[base + i].w - min_bed, 0.0);
        depth_sum += max(eta_rel - rel_bed, 0.0);
    }
    float correction = target_depth_sum - depth_sum;
    vec2 velocity = params.parcel.zw;

    for (uint i = 0u; i < count; ++i) {
        float bed = state_a[base + i].w;
        float h = 0.0;
        if (corridor_cell(i, resolution, center, dir, half_width)) {
            float rel_bed = max(bed - min_bed, 0.0);
            h = max(eta_rel - rel_bed, 0.0);
            if (i == deepest) h = max(h + correction, 0.0);
        }
        vec4 state = vec4(h, h * velocity.x, h * velocity.y, bed);
        state_a[base + i] = state;
        state_b[base + i] = state;
    }
}
