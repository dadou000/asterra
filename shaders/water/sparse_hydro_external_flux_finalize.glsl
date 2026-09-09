#[compute]
#version 450

// Finalize exact fine external source application into the two reserved words of
// SparseHydroStepGPU's existing 96-byte diagnostics control block.
// word 22 / byte 88 = gross added volume [m^3]
// word 23 / byte 92 = gross removed volume [m^3]
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer Partials {
    vec2 partial_flux_m3[];
};
layout(set = 0, binding = 1, std430) buffer ControlWords {
    uint words[];
} control;
layout(set = 0, binding = 2, std430) readonly buffer Params {
    uvec4 config; // total_cells, cells_per_tile, capacity, partial_count
} params;

void main() {
    vec2 total = vec2(0.0);
    for (uint i = 0u; i < params.config.w; ++i) total += partial_flux_m3[i];
    control.words[22] = floatBitsToUint(max(total.x, 0.0));
    control.words[23] = floatBitsToUint(max(total.y, 0.0));
}
