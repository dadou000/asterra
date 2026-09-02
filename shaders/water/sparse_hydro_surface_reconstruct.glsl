#[compute]
#version 450

// Reconstructs the view-local RGBA32F dynamic-water cache from the physical sparse
// atlas. The cache is not authoritative: every output texel maps its planet-space
// direction to the production hydrology quadtree level, resolves the resident slot
// through the ephemeral hash table and samples canonical state A.
//
// Output:
//   R = absolute local free-surface elevation eta=h+bed relative planet mean [m]
//       (0 outside resident wet sparse state, matching mean-ocean baseline)
//   G/B = velocity in the persistent dynamic-cache tangent frame [m/s]
//   A = simple hydraulic activity hint [0..1]

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer State {
    vec4 cells[]; // h, hu, hv, bed
};
layout(set = 0, binding = 1, std430) readonly buffer Occupancy {
    int occupied[];
};
layout(set = 0, binding = 2, std430) readonly buffer TileMetadata {
    ivec4 tile_meta[]; // face, level, x, y
};
layout(set = 0, binding = 3, std430) readonly buffer HashTable {
    uint slots_plus_one[];
};
layout(set = 0, binding = 4, rgba32f) uniform writeonly image2D target_field;
layout(set = 0, binding = 5, std430) readonly buffer Params {
    vec4 dims;         // capacity, tile_res, hydro_level, hash_mask
    vec4 metric;       // planet_radius, dry_eps, target_res, target_half_extent_m
    vec4 anchor_dir;   // xyz
    vec4 anchor_right; // xyz
    vec4 anchor_up;    // xyz
    vec4 view;         // target_center_plane x/y, activity_gain, reserved
} params;

const float CUBE_Q = 0.7853981633974483;
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

vec3 face_axis(int face) {
    if (face == 0) return vec3( 1.0, 0.0, 0.0);
    if (face == 1) return vec3(-1.0, 0.0, 0.0);
    if (face == 2) return vec3(0.0,  1.0, 0.0);
    if (face == 3) return vec3(0.0, -1.0, 0.0);
    if (face == 4) return vec3(0.0, 0.0,  1.0);
    return vec3(0.0, 0.0, -1.0);
}

vec3 face_right(int face) {
    if (face == 0) return vec3(0.0, 0.0, -1.0);
    if (face == 1) return vec3(0.0, 0.0,  1.0);
    if (face == 2) return vec3(-1.0, 0.0, 0.0);
    if (face == 3) return vec3(-1.0, 0.0, 0.0);
    if (face == 4) return vec3( 1.0, 0.0, 0.0);
    return vec3(-1.0, 0.0, 0.0);
}

vec3 face_up(int face) {
    if (face == 0 || face == 1 || face == 4 || face == 5)
        return vec3(0.0, 1.0, 0.0);
    if (face == 2) return vec3(0.0, 0.0,  1.0);
    return vec3(0.0, 0.0, -1.0);
}

void dir_to_face_uv(vec3 input_dir, out int face, out vec2 uv) {
    vec3 d = normalize(input_dir);
    vec3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) face = d.x > 0.0 ? 0 : 1;
    else if (a.y >= a.z) face = d.y > 0.0 ? 2 : 3;
    else face = d.z > 0.0 ? 4 : 5;

    vec3 axis = face_axis(face);
    vec3 right = face_right(face);
    vec3 up = face_up(face);
    float denom = dot(axis, d);
    if (abs(denom) < 1e-8) denom = denom >= 0.0 ? 1e-8 : -1e-8;
    uv = vec2(
        atan(dot(right, d) / denom) / CUBE_Q,
        atan(dot(up, d) / denom) / CUBE_Q);
}

vec3 field_plane_to_planet(vec2 plane_m) {
    vec3 a = normalize(params.anchor_dir.xyz);
    float arc_m = length(plane_m);
    if (arc_m <= 1e-7) return a;
    float radius = max(params.metric.x, 1.0);
    float theta = arc_m / radius;
    vec3 tangent = normalize(
        params.anchor_right.xyz * plane_m.x
        + params.anchor_up.xyz * plane_m.y);
    return normalize(a * cos(theta) + tangent * sin(theta));
}

int find_slot(uvec4 key) {
    uint mask = uint(max(params.dims.w, 0.0) + 0.5);
    uint start = hash_key(key) & mask;
    uint capacity = uint(max(params.dims.x, 1.0) + 0.5);
    for (uint probe = 0u; probe < MAX_PROBES; ++probe) {
        uint value = slots_plus_one[(start + probe) & mask];
        if (value == 0u) return -1;
        uint slot = value - 1u;
        if (slot >= capacity) continue;
        ivec4 raw = tile_meta[slot];
        if (raw.x < 0) continue;
        if (all(equal(uvec4(raw), key)) && occupied[slot] != 0)
            return int(slot);
    }
    return -1;
}

vec2 local_velocity_to_cache(vec3 d, int face, vec2 local_velocity) {
    vec3 fr = face_right(face);
    vec3 fu = face_up(face);
    vec3 local_x = fr - d * dot(fr, d);
    float lx2 = dot(local_x, local_x);
    if (lx2 <= 1e-12) return vec2(0.0);
    local_x *= inversesqrt(lx2);
    vec3 local_y = normalize(cross(d, local_x));
    if (dot(local_y, fu) < 0.0) local_y = -local_y;
    vec3 world_v = local_x * local_velocity.x + local_y * local_velocity.y;

    vec3 cache_x_raw = params.anchor_right.xyz
        - d * dot(params.anchor_right.xyz, d);
    float cx2 = dot(cache_x_raw, cache_x_raw);
    if (cx2 <= 1e-12) return vec2(length(world_v), 0.0);
    vec3 cache_x = cache_x_raw * inversesqrt(cx2);
    vec3 cache_y = normalize(cross(d, cache_x));
    if (dot(cache_y, params.anchor_up.xyz) < 0.0) cache_y = -cache_y;
    return vec2(dot(world_v, cache_x), dot(world_v, cache_y));
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    int target_res = max(int(params.metric.z + 0.5), 1);
    if (pixel.x >= target_res || pixel.y >= target_res) return;

    float half_extent = max(params.metric.w, 1.0);
    vec2 uv_out = (vec2(pixel) + vec2(0.5)) / float(target_res);
    vec2 plane_m = (uv_out * 2.0 - 1.0) * half_extent + params.view.xy;
    vec3 d = field_plane_to_planet(plane_m);

    int face;
    vec2 face_uv;
    dir_to_face_uv(d, face, face_uv);

    int level = max(int(params.dims.z + 0.5), 0);
    int side = 1 << min(level, 27);
    vec2 grid = clamp((face_uv + vec2(1.0)) * 0.5 * float(side),
        vec2(0.0), vec2(float(side) - 1e-5));
    ivec2 tile_xy = ivec2(floor(grid));
    uvec4 key = uvec4(uint(face), uint(level), uvec2(tile_xy));
    int slot = find_slot(key);
    if (slot < 0) {
        imageStore(target_field, pixel, vec4(0.0));
        return;
    }

    int tile_res = max(int(params.dims.y + 0.5), 1);
    vec2 local01 = clamp(grid - vec2(tile_xy), vec2(0.0), vec2(0.999999));
    ivec2 cell_xy = clamp(ivec2(floor(local01 * float(tile_res))),
        ivec2(0), ivec2(tile_res - 1));
    int local_i = cell_xy.y * tile_res + cell_xy.x;
    int i = slot * tile_res * tile_res + local_i;
    vec4 q = cells[i];
    float h = max(q.x, 0.0);
    float dry_eps = max(params.metric.y, 1e-8);
    if (h <= dry_eps || any(isnan(q)) || any(isinf(q))) {
        imageStore(target_field, pixel, vec4(0.0));
        return;
    }

    float eta = q.w + h;
    vec2 local_velocity = q.yz / h;
    vec2 cache_velocity = local_velocity_to_cache(d, face, local_velocity);
    float speed = length(cache_velocity);
    float activity = clamp(speed * max(params.view.z, 0.0), 0.0, 1.0);
    imageStore(target_field, pixel,
        vec4(eta, cache_velocity.x, cache_velocity.y, activity));
}
