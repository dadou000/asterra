#[compute]
#version 450

// Compact source-entry -> sparse source-term buffer writer.
//
// CPU policy resolves world-space emitters to stable tiles/cells and aggregates
// duplicate cell entries before upload, so each destination cell is written by at
// most one invocation and floating-point atomics are unnecessary.
//
// rates per cell:
//   x = added water depth rate      [m/s]
//   y = removed water depth rate    [m/s]
//   z = injected hu rate            [m^2/s^2]
//   w = injected hv rate            [m^2/s^2]

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct SourceEntry {
    uvec4 meta; // slot, local_cell_index, flags, reserved
    vec4 rates;
};

layout(set = 0, binding = 0, std430) readonly buffer Entries {
    SourceEntry entries[];
};
layout(set = 0, binding = 1, std430) writeonly buffer Sources {
    vec4 sources[];
};
layout(set = 0, binding = 2, std430) readonly buffer Occupancy {
    int occupied[];
};
layout(set = 0, binding = 3, std430) readonly buffer Params {
    uvec4 dims; // entry_count, cells_per_tile, capacity, total_cells
} params;

void main() {
    uint entry_i = gl_GlobalInvocationID.x;
    if (entry_i >= params.dims.x) return;

    SourceEntry e = entries[entry_i];
    uint slot = e.meta.x;
    uint local_i = e.meta.y;
    if (slot >= params.dims.z || local_i >= params.dims.y) return;
    if (occupied[slot] == 0) return;

    uint i = slot * params.dims.y + local_i;
    if (i >= params.dims.w) return;
    sources[i] = max(e.rates, vec4(0.0, 0.0, -3.402823e38, -3.402823e38));
}
