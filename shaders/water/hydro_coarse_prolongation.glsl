#[compute]
#version 450

// Terrain-aware coarse -> fine surface-water prolongation for one unpublished tile.
//
// Terrain has already been staged into A+B with h/hu/hv = 0. A single invocation
// solves a level free surface in coordinates relative to the tile's minimum bed:
//
//   sum(max(eta_rel - (bed_i-min_bed), 0)) * cell_area = target_volume
//
// Relative coordinates avoid subtracting two large absolute elevations when water
// is only millimetres/centimetres deep. A fixed bisection finds eta_rel, then the
// remaining FP32 depth-sum residual is applied to the deepest cell. A and B receive
// identical state so atlas-A remains canonical and ping-pong parity is irrelevant.
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) buffer StateA {
    vec4 state_a[];
};
layout(set = 0, binding = 1, std430) buffer StateB {
    vec4 state_b[];
};
layout(set = 0, binding = 2, std430) readonly buffer Params {
    uvec4 config; // slot, cells_per_tile, capacity, bisection_iterations
    vec4 parcel;  // target_volume_m3, cell_area_m2, velocity_u, velocity_v
    vec4 guard;   // max_cells, reserved...
} params;

void main() {
    uint slot = params.config.x;
    uint count = params.config.y;
    if (slot >= params.config.z || count == 0u || count > uint(params.guard.x + 0.5)) return;

    float target_volume = max(params.parcel.x, 0.0);
    float cell_area = max(params.parcel.y, 1e-12);
    if (target_volume <= 0.0) return;

    uint base = slot * count;
    float min_bed = 3.402823466e38;
    float max_bed = -3.402823466e38;
    uint deepest = 0u;
    for (uint i = 0u; i < count; ++i) {
        float bed = state_a[base + i].w;
        if (isnan(bed) || isinf(bed)) return;
        if (bed < min_bed) {
            min_bed = bed;
            deepest = i;
        }
        max_bed = max(max_bed, bed);
    }

    float target_depth_sum = target_volume / cell_area;
    float relief = max(max_bed - min_bed, 0.0);
    float low = 0.0;
    // At this level every cell at max_bed alone already contributes at least the
    // requested depth sum, so the upper bracket is guaranteed to contain a root.
    float high = max(relief + target_depth_sum, 1e-9);
    uint iterations = clamp(params.config.w, 8u, 40u);

    for (uint iter = 0u; iter < iterations; ++iter) {
        float mid = 0.5 * (low + high);
        float depth_sum = 0.0;
        for (uint i = 0u; i < count; ++i) {
            float rel_bed = max(state_a[base + i].w - min_bed, 0.0);
            depth_sum += max(mid - rel_bed, 0.0);
        }
        if (depth_sum < target_depth_sum) low = mid;
        else high = mid;
    }

    // Use the upper bracket so the residual is normally a tiny removal from the
    // deepest cell. The correction makes the serial FP32 depth sum match the target
    // as closely as representable without changing any other wet/dry classification.
    float eta_rel = high;
    float depth_sum = 0.0;
    for (uint i = 0u; i < count; ++i) {
        float rel_bed = max(state_a[base + i].w - min_bed, 0.0);
        depth_sum += max(eta_rel - rel_bed, 0.0);
    }
    float correction = target_depth_sum - depth_sum;

    vec2 velocity = params.parcel.zw;
    for (uint i = 0u; i < count; ++i) {
        float bed = state_a[base + i].w;
        float rel_bed = max(bed - min_bed, 0.0);
        float h = max(eta_rel - rel_bed, 0.0);
        if (i == deepest) h = max(h + correction, 0.0);
        vec4 state = vec4(h, h * velocity.x, h * velocity.y, bed);
        state_a[base + i] = state;
        state_b[base + i] = state;
    }
}
