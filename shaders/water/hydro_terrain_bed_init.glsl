#[compute]
#version 450

// Initialize one reserved sparse-hydrology slot directly from Asterra's resident
// terrain representation. The destination remains unpublished (occupancy=0).
//
// The pristine height evaluator mirrors gpu_terrain_height.gdshaderinc /
// GroundHeightStore. A tiny per-hydrology-tile delta patch is supplied separately
// for sparse player edits. No full bed/state tile is uploaded from CPU.
//
// Outputs for every cell in the reserved slot:
//   state A = vec4(0, 0, 0, bed)
//   state B = vec4(0, 0, 0, bed)
//   source  = vec4(0)

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) writeonly buffer StateA {
    vec4 state_a[];
};
layout(set = 0, binding = 1, std430) writeonly buffer StateB {
    vec4 state_b[];
};
layout(set = 0, binding = 2, std430) writeonly buffer Sources {
    vec4 sources[];
};
layout(set = 0, binding = 3, std430) readonly buffer DeltaOffsets {
    float delta_offsets[];
};
layout(set = 0, binding = 4, std430) readonly buffer Params {
    uvec4 tile;   // face, key_level, key_x, key_y
    uvec4 target; // slot, tile_res, terrain_level, capacity
    vec4 terrain; // planet_radius, base_spacing, detail_strength, macro_face_res
    uvec4 misc;   // detail_seed, macro_ready, max_macro_mip, reserved
} params;
layout(set = 0, binding = 5) uniform sampler2DArray macro_elevation;

const float Q = 0.7853981633974483; // PI / 4
const int MAX_TERRAIN_LEVEL = 14;

vec3 face_axis(uint face) {
    if (face == 0u) return vec3( 1.0, 0.0, 0.0);
    if (face == 1u) return vec3(-1.0, 0.0, 0.0);
    if (face == 2u) return vec3(0.0,  1.0, 0.0);
    if (face == 3u) return vec3(0.0, -1.0, 0.0);
    if (face == 4u) return vec3(0.0, 0.0,  1.0);
    return vec3(0.0, 0.0, -1.0);
}

vec3 face_right(uint face) {
    if (face == 0u) return vec3(0.0, 0.0, -1.0);
    if (face == 1u) return vec3(0.0, 0.0,  1.0);
    if (face == 2u) return vec3(-1.0, 0.0, 0.0);
    if (face == 3u) return vec3(-1.0, 0.0, 0.0);
    if (face == 4u) return vec3( 1.0, 0.0, 0.0);
    return vec3(-1.0, 0.0, 0.0);
}

vec3 face_up(uint face) {
    if (face == 0u || face == 1u || face == 4u || face == 5u)
        return vec3(0.0, 1.0, 0.0);
    if (face == 2u) return vec3(0.0, 0.0,  1.0);
    return vec3(0.0, 0.0, -1.0);
}

vec3 face_uv_to_dir(uint face, float u, float v) {
    float tu = tan(u * Q);
    float tv = tan(v * Q);
    return normalize(face_axis(face) + face_right(face) * tu + face_up(face) * tv);
}

vec3 surface_face_uv(vec3 dir) {
    vec3 d = normalize(dir);
    vec3 ad = abs(d);
    uint face;
    if (ad.x >= ad.y && ad.x >= ad.z) face = d.x >= 0.0 ? 0u : 1u;
    else if (ad.y >= ad.z) face = d.y >= 0.0 ? 2u : 3u;
    else face = d.z >= 0.0 ? 4u : 5u;
    vec3 axis = face_axis(face);
    float denom = max(dot(axis, d), 1e-8);
    float u = atan(dot(face_right(face), d) / denom) / Q;
    float v = atan(dot(face_up(face), d) / denom) / Q;
    return vec3(u * 0.5 + 0.5, v * 0.5 + 0.5, float(face));
}

float smootherstep01(float x) {
    x = clamp(x, 0.0, 1.0);
    return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

float smootherstep_range(float a, float b, float x) {
    return smootherstep01((x - a) / max(b - a, 1e-9));
}

vec4 cubic_bspline_weights(float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return vec4(
        (1.0 - 3.0*t + 3.0*t2 - t3) / 6.0,
        (4.0 - 6.0*t2 + 3.0*t3) / 6.0,
        (1.0 + 3.0*t + 3.0*t2 - 3.0*t3) / 6.0,
        t3 / 6.0);
}

vec3 macro_array_coord(vec3 dir) {
    vec3 fuv = surface_face_uv(dir);
    ivec3 size3 = textureSize(macro_elevation, 0);
    float texture_res = float(size3.x);
    float face_res = max(params.terrain.w, 1.0);
    float gutter = max((texture_res - face_res) * 0.5, 0.0);
    vec2 uv = (fuv.xy * face_res + vec2(gutter)) / texture_res;
    return vec3(uv, fuv.z);
}

float macro_bicubic_mip(vec3 coord, int mip) {
    ivec3 size3 = textureSize(macro_elevation, mip);
    vec2 size2 = vec2(size3.xy);
    vec2 p = coord.xy * size2 - vec2(0.5);
    vec2 base = floor(p);
    vec2 f = fract(p);
    vec4 wx = cubic_bspline_weights(f.x);
    vec4 wy = cubic_bspline_weights(f.y);
    float gx0 = wx.x + wx.y;
    float gx1 = wx.z + wx.w;
    float gy0 = wy.x + wy.y;
    float gy1 = wy.z + wy.w;
    float x0 = base.x - 1.0 + wx.y / max(gx0, 1e-9);
    float x1 = base.x + 1.0 + wx.w / max(gx1, 1e-9);
    float y0 = base.y - 1.0 + wy.y / max(gy0, 1e-9);
    float y1 = base.y + 1.0 + wy.w / max(gy1, 1e-9);
    vec2 uv00 = (vec2(x0, y0) + vec2(0.5)) / size2;
    vec2 uv10 = (vec2(x1, y0) + vec2(0.5)) / size2;
    vec2 uv01 = (vec2(x0, y1) + vec2(0.5)) / size2;
    vec2 uv11 = (vec2(x1, y1) + vec2(0.5)) / size2;
    float fmip = float(mip);
    float s00 = textureLod(macro_elevation, vec3(uv00, coord.z), fmip).r;
    float s10 = textureLod(macro_elevation, vec3(uv10, coord.z), fmip).r;
    float s01 = textureLod(macro_elevation, vec3(uv01, coord.z), fmip).r;
    float s11 = textureLod(macro_elevation, vec3(uv11, coord.z), fmip).r;
    float row0 = mix(s00, s10, gx1);
    float row1 = mix(s01, s11, gx1);
    return mix(row0, row1, gy1);
}

float macro_height(vec3 d, int level) {
    if (params.misc.y == 0u || params.terrain.w <= 0.5) return 0.0;
    float radius = max(params.terrain.x, 1.0);
    float spacing = max(params.terrain.y, 1e-4) * exp2(float(level));
    float macro_texel_m = (1.5707963267948966 * radius) / max(params.terrain.w, 1.0);
    float footprint_m = max(spacing * 3.0, macro_texel_m);
    float lod = clamp(log2(footprint_m / macro_texel_m), 0.0, float(params.misc.z));
    int mip0 = clamp(int(floor(lod)), 0, int(params.misc.z));
    int mip1 = min(mip0 + 1, int(params.misc.z));
    float t = fract(lod);
    vec3 coord = macro_array_coord(d);
    float h0 = macro_bicubic_mip(coord, mip0);
    if (mip0 == mip1 || t <= 0.001) return h0;
    float h1 = macro_bicubic_mip(coord, mip1);
    return mix(h0, h1, smootherstep01(t));
}

uint terrain_hash(ivec3 p, uint seed) {
    uint h = uint(p.x) * 0x8da6b343u;
    h ^= uint(p.y) * 0xd8163841u;
    h ^= uint(p.z) * 0xcb1ab31fu;
    h ^= seed * 0x9e3779b9u;
    h ^= h >> 16u; h *= 0x7feb352du;
    h ^= h >> 15u; h *= 0x846ca68bu;
    h ^= h >> 16u;
    return h;
}

float lattice_value(ivec3 p, uint seed) {
    uint h = terrain_hash(p, seed) & 0x00ffffffu;
    return float(h) * (2.0 / 16777215.0) - 1.0;
}

float smooth_value_noise(vec3 p, uint seed) {
    ivec3 i = ivec3(floor(p));
    vec3 f = fract(p);
    f = f*f*f*(f*(f*6.0 - 15.0) + 10.0);
    float n000 = lattice_value(i + ivec3(0,0,0), seed);
    float n100 = lattice_value(i + ivec3(1,0,0), seed);
    float n010 = lattice_value(i + ivec3(0,1,0), seed);
    float n110 = lattice_value(i + ivec3(1,1,0), seed);
    float n001 = lattice_value(i + ivec3(0,0,1), seed);
    float n101 = lattice_value(i + ivec3(1,0,1), seed);
    float n011 = lattice_value(i + ivec3(0,1,1), seed);
    float n111 = lattice_value(i + ivec3(1,1,1), seed);
    float nx00 = mix(n000, n100, f.x);
    float nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x);
    float nx11 = mix(n011, n111, f.x);
    return mix(mix(nx00, nx10, f.y), mix(nx01, nx11, f.y), f.z);
}

float band_weight(float wavelength, float spacing) {
    return smootherstep_range(spacing * 4.0, spacing * 8.0, wavelength);
}

float detail_band(vec3 world_m, float wavelength, float spacing, uint seed, float amplitude) {
    float w = band_weight(wavelength, spacing);
    if (w <= 0.001) return 0.0;
    return smooth_value_noise(world_m / wavelength, seed) * amplitude * w;
}

float procedural_detail(vec3 d, int level, float macro_h) {
    float spacing = max(params.terrain.y, 1e-4) * exp2(float(level));
    vec3 world_m = d * max(params.terrain.x, 1.0);
    uint seed = max(params.misc.x, 1u);
    float coast_guard = mix(0.38, 1.0, smootherstep_range(45.0, 320.0, abs(macro_h)));
    float mountain = smootherstep_range(500.0, 2600.0, max(macro_h, 0.0));
    float regional_gain = mix(0.72, 1.30, mountain);
    float h = 0.0;
    h += detail_band(world_m, 12000.0, spacing, seed + 11u, mix(52.0, 115.0, mountain));
    float w2400 = band_weight(2400.0, spacing);
    if (w2400 > 0.001) {
        float n = smooth_value_noise(world_m / 2400.0, seed + 23u);
        float ridge = (1.0 - abs(n)) * 2.0 - 1.0;
        h += (n * 28.0 + ridge * 18.0 * mountain) * w2400;
    }
    h += detail_band(world_m, 480.0, spacing, seed + 37u, mix(7.0, 15.0, mountain));
    h += detail_band(world_m, 96.0, spacing, seed + 53u, 3.4);
    h += detail_band(world_m, 24.0, spacing, seed + 71u, 0.85);
    return h * coast_guard * regional_gain * max(params.terrain.z, 0.0);
}

void main() {
    uvec2 p = gl_GlobalInvocationID.xy;
    uint r = max(params.target.y, 1u);
    uint slot = params.target.x;
    uint capacity = max(params.target.w, 1u);
    if (p.x >= r || p.y >= r || slot >= capacity || params.tile.x >= 6u) return;

    uint side = 1u << min(params.tile.y, 27u);
    float fx = (float(params.tile.z) + (float(p.x) + 0.5) / float(r)) / float(side);
    float fy = (float(params.tile.w) + (float(p.y) + 0.5) / float(r)) / float(side);
    float u = fx * 2.0 - 1.0;
    float v = fy * 2.0 - 1.0;
    vec3 d = face_uv_to_dir(params.tile.x, u, v);

    int terrain_level = clamp(int(params.target.z), 0, MAX_TERRAIN_LEVEL);
    float macro_h = macro_height(d, terrain_level);
    float bed = macro_h + procedural_detail(d, terrain_level, macro_h);
    uint local_i = p.y * r + p.x;
    bed += delta_offsets[local_i];

    uint i = slot * r * r + local_i;
    vec4 dry = vec4(0.0, 0.0, 0.0, bed);
    state_a[i] = dry;
    state_b[i] = dry;
    sources[i] = vec4(0.0);
}
