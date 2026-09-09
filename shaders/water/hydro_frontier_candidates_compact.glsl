#[compute]
#version 450

// Frontier generation over the dense temporal activity queue instead of atlas
// capacity. Each compact activity entry is exactly 21 uints:
//   slot + floatBits(5 * vec4 HydroTileActivity summary).

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

struct FrontierEntry {
    uvec4 a; // slot, direction, floatBitsToUint(effective_flux), face
    uvec4 b; // level, x, y, flags(bit0=predicted dominated)
    uvec4 c; // floatBitsToUint(source_edge_max_eta), reserved...
};

layout(set = 0, binding = 0, std430) readonly buffer CompactActivity {
    uint activity_words[];
};
layout(set = 0, binding = 1, std430) readonly buffer TileMetadata {
    uvec4 tile_meta[];
};
layout(set = 0, binding = 2, std430) buffer Queue {
    uint count;
    uint overflow;
    uint reserved0;
    uint reserved1;
    FrontierEntry entries[];
} queue_out;
layout(set = 0, binding = 3, std430) readonly buffer Params {
    vec4 config; // atlas capacity, threshold_m3s, max_candidates, optional tile_resolution
} params;
layout(set = 0, binding = 4, std430) readonly buffer State {
    vec4 cells[];
};

const uint HEADER_WORDS = 4u;
const uint ENTRY_WORDS = 21u;

vec4 load_vec4(uint base) {
    return vec4(
        uintBitsToFloat(activity_words[base + 0u]),
        uintBitsToFloat(activity_words[base + 1u]),
        uintBitsToFloat(activity_words[base + 2u]),
        uintBitsToFloat(activity_words[base + 3u]));
}

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
    uint compact_index = gl_GlobalInvocationID.x;
    uint active_count = activity_words[0];
    if (compact_index >= active_count) return;

    uint entry = HEADER_WORDS + compact_index * ENTRY_WORDS;
    uint slot = activity_words[entry + 0u];
    uint capacity = max(uint(params.config.x + 0.5), 1u);
    if (slot >= capacity) return;

    vec4 health = load_vec4(entry + 1u);
    vec4 edges = load_vec4(entry + 5u);
    vec4 ownership = load_vec4(entry + 9u);
    vec4 wetting = load_vec4(entry + 13u);
    vec4 edge_eta = load_vec4(entry + 17u);
    uvec4 meta = tile_meta[slot];
    if (ownership.y < 0.5 || health.w > 0.5 || meta.x == 0xffffffffu) return;

    float threshold = max(params.config.y, 0.0);
    uint max_candidates = max(uint(params.config.z + 0.5), 1u);
    maybe_emit(slot, 0u, edges.x, wetting.x, edge_eta.x, threshold, meta, max_candidates);
    maybe_emit(slot, 1u, edges.y, wetting.y, edge_eta.y, threshold, meta, max_candidates);
    maybe_emit(slot, 2u, edges.z, wetting.z, edge_eta.z, threshold, meta, max_candidates);
    maybe_emit(slot, 3u, edges.w, wetting.w, edge_eta.w, threshold, meta, max_candidates);
}
