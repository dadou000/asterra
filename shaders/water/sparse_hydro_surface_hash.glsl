#[compute]
#version 450

// Builds an ephemeral render-cache lookup from stable sparse tile metadata to
// transient atlas slots. The table is cleared before every rebuild; value 0 means
// empty and value slot+1 points back into TileMetadata for exact collision checks.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer Occupancy {
    int occupied[];
};
layout(set = 0, binding = 1, std430) readonly buffer TileMetadata {
    ivec4 tile_meta[]; // face, level, x, y
};
layout(set = 0, binding = 2, std430) buffer HashTable {
    uint slots_plus_one[];
};
layout(set = 0, binding = 3, std430) readonly buffer Params {
    vec4 dims; // capacity, tile_res, hydro_level, hash_mask
    vec4 metric;
    vec4 anchor_dir;
    vec4 anchor_right;
    vec4 anchor_up;
    vec4 view;
} params;

const uint MAX_PROBES = 32u;

uint hash_key(uvec4 key) {
    uint h = key.x * 0x9e3779b9u;
    h ^= key.y * 0x85ebca6bu;
    h ^= key.z * 0xc2b2ae35u;
    h ^= key.w * 0x27d4eb2fu;
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    h ^= h >> 16u;
    return h;
}

void main() {
    uint slot = gl_GlobalInvocationID.x;
    uint capacity = uint(max(params.dims.x, 1.0) + 0.5);
    if (slot >= capacity || occupied[slot] == 0) return;

    ivec4 raw = tile_meta[slot];
    if (raw.x < 0) return;
    uvec4 key = uvec4(raw);
    uint mask = uint(max(params.dims.w, 0.0) + 0.5);
    uint start = hash_key(key) & mask;
    uint value = slot + 1u;

    for (uint probe = 0u; probe < MAX_PROBES; ++probe) {
        uint index = (start + probe) & mask;
        uint old = atomicCompSwap(slots_plus_one[index], 0u, value);
        if (old == 0u || old == value) return;
    }
}
