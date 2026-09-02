#[compute]
#version 450

// Converts compact per-tile summaries into a still-smaller frontier queue.
// Every candidate snapshots stable identity beside the transient source slot so a
// delayed async result can be rejected after slot recycling.
//
// Actual advective discharge and predictive dry-neighbor wetting discharge are
// separate in the summary. The queue uses the larger value for activation; entry
// flag bit0 records whether the predictive term dominated.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct FrontierEntry {
    uvec4 a; // slot, direction, floatBitsToUint(effective_flux), face
    uvec4 b; // level, x, y, flags(bit0=predicted dominated)
};

layout(set = 0, binding = 0, std430) readonly buffer Summaries {
    vec4 summary[]; // 4 vec4 per slot
};
layout(set = 0, binding = 1, std430) readonly buffer TileMetadata {
    uvec4 tile_meta[]; // face, level, x, y; face=0xffffffff means unbound
};
layout(set = 0, binding = 2, std430) buffer Queue {
    uint count;
    uint overflow;
    uint reserved0;
    uint reserved1;
    FrontierEntry entries[];
} queue_out;
layout(set = 0, binding = 3, std430) readonly buffer Params {
    vec4 config; // capacity, threshold_m3s, max_candidates, reserved
} params;

void emit_candidate(uint slot, uint direction, float actual_flux,
        float wetting_flux, uvec4 meta, uint max_candidates) {
    float effective_flux = max(actual_flux, wetting_flux);
    uint index = atomicAdd(queue_out.count, 1u);
    if (index >= max_candidates) {
        atomicOr(queue_out.overflow, 1u);
        return;
    }
    uint flags = wetting_flux > actual_flux ? 1u : 0u;
    queue_out.entries[index].a = uvec4(
        slot, direction, floatBitsToUint(effective_flux), meta.x);
    queue_out.entries[index].b = uvec4(meta.y, meta.z, meta.w, flags);
}

void maybe_emit(uint slot, uint direction, float actual_flux, float wetting_flux,
        float threshold, uvec4 meta, uint max_candidates) {
    if (max(actual_flux, wetting_flux) > threshold)
        emit_candidate(slot, direction, actual_flux, wetting_flux, meta, max_candidates);
}

void main() {
    uint slot = gl_GlobalInvocationID.x;
    uint capacity = max(uint(params.config.x + 0.5), 1u);
    uint max_candidates = max(uint(params.config.z + 0.5), 1u);
    if (slot >= capacity) return;

    uint o = slot * 4u;
    vec4 health = summary[o + 0u];
    vec4 edges = summary[o + 1u];
    vec4 ownership = summary[o + 2u];
    vec4 wetting = summary[o + 3u];
    uvec4 meta = tile_meta[slot];
    if (ownership.y < 0.5 || health.w > 0.5 || meta.x == 0xffffffffu) return;

    float threshold = max(params.config.y, 0.0);
    maybe_emit(slot, 0u, edges.x, wetting.x, threshold, meta, max_candidates);
    maybe_emit(slot, 1u, edges.y, wetting.y, threshold, meta, max_candidates);
    maybe_emit(slot, 2u, edges.z, wetting.z, threshold, meta, max_candidates);
    maybe_emit(slot, 3u, edges.w, wetting.w, threshold, meta, max_candidates);
}
