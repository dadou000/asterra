#[compute]
#version 450

// Compact occupancy-aware reduction for one authoritative sparse tile.
//
// Output per workgroup:
//   x = water volume [m^3]
//   y = integral(hu dA) [m^4/s]
//   z = integral(hv dA) [m^4/s]
//   w = max depth [m]
//
// The integrated momentum components divided by volume yield the depth/volume-
// weighted mean local tangent velocity. They are diagnostics/reconstruction hints;
// the persistent 1D reach remains volume-authoritative after collapse.

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer State {
    vec4 cells[]; // h, hu, hv, bed
};
layout(set = 0, binding = 1, std430) readonly buffer Occupancy {
    int occupied[];
};
layout(set = 0, binding = 2, std430) writeonly buffer Partials {
    vec4 partials[];
};
layout(set = 0, binding = 3, std430) readonly buffer Params {
    uvec4 layout_u; // cells_per_tile, capacity, slot, partial_count
    vec4 metric;    // cell_size_m, reserved...
} params;

shared vec4 scratch[256];

void main() {
    uint lane = gl_LocalInvocationID.x;
    uint group = gl_WorkGroupID.x;
    uint local_index = group * 256u + lane;
    uint cells_per_tile = params.layout_u.x;
    uint capacity = params.layout_u.y;
    uint slot = params.layout_u.z;
    float dx = max(params.metric.x, 1e-6);
    float area = dx * dx;

    vec4 value = vec4(0.0);
    if (slot < capacity && occupied[slot] != 0 && local_index < cells_per_tile) {
        uint i = slot * cells_per_tile + local_index;
        vec4 s = cells[i];
        float h = max(s.x, 0.0);
        value = vec4(h * area, s.y * area, s.z * area, h);
    }
    scratch[lane] = value;
    barrier();

    for (uint stride = 128u; stride > 0u; stride >>= 1u) {
        if (lane < stride) {
            scratch[lane].xyz += scratch[lane + stride].xyz;
            scratch[lane].w = max(scratch[lane].w, scratch[lane + stride].w);
        }
        barrier();
    }

    if (lane == 0u)
        partials[group] = scratch[0];
}
