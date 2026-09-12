#[compute]
#version 450

// One-shot conservative seed for an unpublished sparse-hydrology slot.
// Terrain staging must already have initialized both ping-pong states.
// The same depth/momentum parcel is added to A and B so the first solver step can
// start from either parity without changing ownership or mass.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) buffer StateA {
    vec4 state_a[]; // h, hu, hv, bed
};
layout(set = 0, binding = 1, std430) buffer StateB {
    vec4 state_b[];
};
layout(set = 0, binding = 2, std430) readonly buffer Params {
    uvec4 target; // slot, tile_res, capacity, reserved
    vec4 seed;    // uniform depth [m], local u [m/s], local v [m/s], reserved
} params;

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    uint r = max(params.target.y, 1u);
    uint slot = params.target.x;
    uint capacity = max(params.target.z, 1u);
    if (p.x >= r || p.y >= r || slot >= capacity) return;

    float dh = max(params.seed.x, 0.0);
    vec4 parcel = vec4(dh, dh * params.seed.y, dh * params.seed.z, 0.0);
    uint local_i = p.y * r + p.x;
    uint i = slot * r * r + local_i;
    state_a[i] += parcel;
    state_b[i] += parcel;
}
