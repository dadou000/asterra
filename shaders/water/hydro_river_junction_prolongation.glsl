#[compute]
#version 450

// Exact-volume terrain-aware prolongation for a branched river junction tile.
// The wet mask is the union of up to eight incoming corridor segments plus one
// outgoing segment. One common free surface is solved over that union so a seeded
// confluence is hydraulically connected from publication frame zero.

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) buffer StateA { vec4 state_a[]; };
layout(set = 0, binding = 1, std430) buffer StateB { vec4 state_b[]; };

struct JunctionSegment {
    vec4 line; // start_x, start_y, end_x, end_y in tile-cell coordinates
    vec4 flow; // half_width_cells, velocity_x, velocity_y, reserved
};

layout(set = 0, binding = 2, std430) readonly buffer Params {
    uvec4 config; // slot, cells_per_tile, tile_resolution, capacity
    uvec4 counts; // segment_count, bisection_iterations, max_cells, reserved
    vec4 parcel;  // target_volume_m3, cell_area_m2, reserved, reserved
    JunctionSegment segments[9];
} params;

float distance_to_segment(vec2 p, vec2 a, vec2 b) {
    vec2 ab = b - a;
    float denom = dot(ab, ab);
    if (denom <= 1e-12) return length(p - a);
    float t = clamp(dot(p - a, ab) / denom, 0.0, 1.0);
    return length(p - (a + ab * t));
}

bool junction_cell(vec2 p, uint segment_count) {
    for (uint s = 0u; s < segment_count; ++s) {
        JunctionSegment seg = params.segments[s];
        if (distance_to_segment(p, seg.line.xy, seg.line.zw) <= max(seg.flow.x, 0.51))
            return true;
    }
    return false;
}

vec2 velocity_for_cell(vec2 p, uint segment_count) {
    float best = 3.402823466e38;
    vec2 velocity = vec2(0.0);
    for (uint s = 0u; s < segment_count; ++s) {
        JunctionSegment seg = params.segments[s];
        float d = distance_to_segment(p, seg.line.xy, seg.line.zw);
        if (d < best) {
            best = d;
            velocity = seg.flow.yz;
        }
    }
    return velocity;
}

void main() {
    uint slot = params.config.x;
    uint count = params.config.y;
    uint resolution = params.config.z;
    uint capacity = params.config.w;
    uint segment_count = params.counts.x;
    uint iterations = clamp(params.counts.y, 8u, 40u);
    uint max_cells = params.counts.z;
    if (slot >= capacity || count == 0u || resolution == 0u ||
            count != resolution * resolution || count > max_cells ||
            segment_count < 2u || segment_count > 9u) return;

    float target_volume = max(params.parcel.x, 0.0);
    float cell_area = max(params.parcel.y, 1e-12);
    if (target_volume <= 0.0) return;

    uint base = slot * count;
    float min_bed = 3.402823466e38;
    float max_bed = -3.402823466e38;
    uint deepest = 0u;
    uint eligible = 0u;
    for (uint i = 0u; i < count; ++i) {
        uint x = i % resolution;
        uint y = i / resolution;
        vec2 p = vec2(float(x) + 0.5, float(y) + 0.5);
        if (!junction_cell(p, segment_count)) continue;
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
    for (uint iter = 0u; iter < iterations; ++iter) {
        float mid = 0.5 * (low + high);
        float depth_sum = 0.0;
        for (uint i = 0u; i < count; ++i) {
            uint x = i % resolution;
            uint y = i / resolution;
            vec2 p = vec2(float(x) + 0.5, float(y) + 0.5);
            if (!junction_cell(p, segment_count)) continue;
            float rel_bed = max(state_a[base + i].w - min_bed, 0.0);
            depth_sum += max(mid - rel_bed, 0.0);
        }
        if (depth_sum < target_depth_sum) low = mid;
        else high = mid;
    }

    float eta_rel = high;
    float depth_sum = 0.0;
    for (uint i = 0u; i < count; ++i) {
        uint x = i % resolution;
        uint y = i / resolution;
        vec2 p = vec2(float(x) + 0.5, float(y) + 0.5);
        if (!junction_cell(p, segment_count)) continue;
        float rel_bed = max(state_a[base + i].w - min_bed, 0.0);
        depth_sum += max(eta_rel - rel_bed, 0.0);
    }
    float correction = target_depth_sum - depth_sum;

    for (uint i = 0u; i < count; ++i) {
        uint x = i % resolution;
        uint y = i / resolution;
        vec2 p = vec2(float(x) + 0.5, float(y) + 0.5);
        float bed = state_a[base + i].w;
        float h = 0.0;
        vec2 velocity = vec2(0.0);
        if (junction_cell(p, segment_count)) {
            float rel_bed = max(bed - min_bed, 0.0);
            h = max(eta_rel - rel_bed, 0.0);
            if (i == deepest) h = max(h + correction, 0.0);
            velocity = velocity_for_cell(p, segment_count);
        }
        vec4 q = vec4(h, h * velocity.x, h * velocity.y, bed);
        state_a[base + i] = q;
        state_b[base + i] = q;
    }
}
