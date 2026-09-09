#[compute]
#version 450

layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer Partials {
    vec4 partials[];
};
layout(set = 0, binding = 1, std430) writeonly buffer Result {
    vec4 result_state[];
};
layout(set = 0, binding = 2, std430) readonly buffer Params {
    uvec4 layout_u; // cells_per_tile, capacity, slot, partial_count
    vec4 metric;
} params;

void main() {
    vec3 sums = vec3(0.0);
    float max_depth = 0.0;
    for (uint i = 0u; i < params.layout_u.w; ++i) {
        vec4 p = partials[i];
        sums += p.xyz;
        max_depth = max(max_depth, p.w);
    }
    result_state[0] = vec4(sums, max_depth);
}
