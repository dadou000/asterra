#[compute]
#version 450

// Compact active 80-byte HydroTileActivity summaries into a dense CPU/frontier queue.
// Queue layout is deliberately scalar/raw so one entry is exactly 84 bytes:
//   header uint[4] = count, overflow, reserved, reserved
//   entry  uint[21] = slot + floatBits(summary[5 * vec4])
// The dense queue is production-temporal only; the canonical 5-vec4 summary ABI
// remains unchanged for the fused cache and any legacy GPU consumers.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer ActivitySummaries {
    vec4 summary[];
};
layout(set = 0, binding = 1, std430) buffer CompactQueue {
    uint words[];
};
layout(set = 0, binding = 2, std430) readonly buffer Params {
    uvec4 config; // capacity, max_entries, reserved, reserved
} params;

const uint HEADER_WORDS = 4u;
const uint ENTRY_WORDS = 21u;

void store_vec4(uint base, vec4 v) {
    words[base + 0u] = floatBitsToUint(v.x);
    words[base + 1u] = floatBitsToUint(v.y);
    words[base + 2u] = floatBitsToUint(v.z);
    words[base + 3u] = floatBitsToUint(v.w);
}

void main() {
    uint slot = gl_GlobalInvocationID.x;
    uint capacity = max(params.config.x, 1u);
    if (slot >= capacity) return;

    uint source = slot * 5u;
    vec4 ownership = summary[source + 2u];
    if (ownership.y < 0.5) return;

    uint index = atomicAdd(words[0], 1u);
    uint max_entries = max(params.config.y, 1u);
    if (index >= max_entries) {
        atomicOr(words[1], 1u);
        return;
    }

    uint out_base = HEADER_WORDS + index * ENTRY_WORDS;
    words[out_base] = slot;
    store_vec4(out_base + 1u, summary[source + 0u]);
    store_vec4(out_base + 5u, summary[source + 1u]);
    store_vec4(out_base + 9u, summary[source + 2u]);
    store_vec4(out_base + 13u, summary[source + 3u]);
    store_vec4(out_base + 17u, summary[source + 4u]);
}
