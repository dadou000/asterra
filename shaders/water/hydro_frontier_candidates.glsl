#[compute]
#version 450

// Converts compact per-tile summaries into a still-smaller frontier queue.
// Every candidate snapshots stable identity beside the transient source slot so a
// delayed async result can be rejected after slot recycling.
//
// Actual advective discharge and predictive dry-neighbor wetting discharge are
// separate in the summary. The queue uses the larger value for activation; entry
// flag bit0 records whether the predictive term dominated.
//
// Tile summaries carry full-FP32 max(h+bed) for W/E/S/N. An optional direct state
// scan can cross-check that edge head when callers provide state+tile_resolution;
// ordinary runtime use needs only the compact summary buffer.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct FrontierEntry {
    uvec4 a; // slot, direction, floatBitsToUint(effective_flux), face
    uvec4 b; // level, x, y, flags(bit0=predicted dominated)
    uvec4 c; // floatBitsToUint(source_edge_max_eta), reserved...
};

layout(set = 0, binding = 0, std430) readonly buffer Summaries {
    vec4 summary[]; // 5 vec4 per slot
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
    vec4 config; // capacity, threshold_m3s, max_candidates, optional tile_resolution
} params;
layout(set = 0, binding = 4, std430) readonly buffer State {
    vec4 cells[]; // optional canonical h,hu,hv,bed; may alias summary when tile_res=0
};

float source_edge_max_eta(uint slot, uint direction, float summarized_eta) {
    uint r = uint(max(params.config.w, 0.0) + 0.5);
    if (r == 0u) return summarized_eta;
    uint base = slot * r * r;
    float eta = summarized_eta;
    for (uint k = 0u; k < r; ++k) {
        uint local_i;
        if (direction == 0u) local_i = k * r;
        else if (direction == 1u) local_i = k * r + (r - 1u);
        else if (direction == 2u) local_i = k;
        else local_i = (r - 1u) * r + k;
        vec4 q = cells[base + local_i];
        if (any(isnan(q)) || any(isinf(q))) continue;
        eta = max(eta, max(q.x, 0.0) + q.w);
    }
    return eta;
}

void emit_candidate(uint slot, uint direction, float actual_flux,
        float wetting_flux, float summarized_eta, uvec4 meta, uint max_candidates) {
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
    queue_out.entries[index].c = uvec4(
        floatBitsToUint(source_edge_max_eta(slot, direction, summarized_eta)),
        0u, 0u, 0u);
}

void maybe_emit(uint slot, uint direction, float actual_flux, float wetting_flux,
        float edge_eta, float threshold, uvec4 meta, uint max_candidates) {
    if (max(actual_flux, wetting_flux) > threshold)
        emit_candidate(slot, direction, actual_flux, wetting_flux,
            edge_eta, meta, max_candidates);
}

void main() {
    uint slot = gl_GlobalInvocationID.x;
    uint capacity = max(uint(params.config.x + 0.5), 1u);
    uint max_candidates = max(uint(params.config.z + 0.5), 1u);
    if (slot >= capacity) return;

    uint o = slot * 5u;
    vec4 health = summary[o + 0u];
    vec4 edges = summary[o + 1u];
    vec4 ownership = summary[o + 2u];
    vec4 wetting = summary[o + 3u];
    vec4 edge_eta = summary[o + 4u];
    uvec4 meta = tile_meta[slot];
    if (ownership.y < 0.5 || health.w > 0.5 || meta.x == 0xffffffffu) return;

    float threshold = max(params.config.y, 0.0);
    maybe_emit(slot, 0u, edges.x, wetting.x, edge_eta.x, threshold, meta, max_candidates);
    maybe_emit(slot, 1u, edges.y, wetting.y, edge_eta.y, threshold, meta, max_candidates);
    maybe_emit(slot, 2u, edges.z, wetting.z, edge_eta.z, threshold, meta, max_candidates);
    maybe_emit(slot, 3u, edges.w, wetting.w, edge_eta.w, threshold, meta, max_candidates);
}
