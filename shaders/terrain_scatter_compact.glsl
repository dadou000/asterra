#[compute]
#version 450

// Exact near-field terrain scatter suitability + compaction.
//
// One invocation owns one deterministic candidate cell. Unsuitable candidates
// return before writing anything. Accepted candidates atomically reserve one slot
// in the MultiMesh indirect command buffer and write a packed 3D transform plus
// INSTANCE_CUSTOM data directly into the MultiMesh storage buffer.
//
// The transform layout is Godot's 3D MultiMesh + custom-data layout:
//   0: basis.x.x, basis.y.x, basis.z.x, origin.x
//   1: basis.x.y, basis.y.y, basis.z.y, origin.y
//   2: basis.x.z, basis.y.z, basis.z.z, origin.z
//   3: custom.r, custom.g, custom.b, custom.a
//
// family: 0 grass, 1 geological/scree stone, 2 river/depositional stone.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2DArray macro_tex;
layout(set = 0, binding = 1) uniform sampler2DArray soil_tex;
layout(set = 0, binding = 2) uniform sampler2DArray surface_tex;
layout(set = 0, binding = 3) uniform sampler2DArray geology_tex;
layout(set = 0, binding = 4) uniform sampler2DArray structure_tex;
layout(set = 0, binding = 5) uniform sampler2DArray climate_tex;
layout(set = 0, binding = 6) uniform sampler2DArray hydrology_tex;
layout(set = 0, binding = 7) uniform sampler2DArray rock_tex;
layout(set = 0, binding = 8) uniform sampler2DArray biome_tex;

layout(set = 0, binding = 9, std430) restrict writeonly buffer InstanceBuffer {
    vec4 data[];
} instance_buffer;

// Godot indexed MultiMesh indirect command layout:
// 0 indexCount, 1 instanceCount, 2 firstIndex, 3 vertexOffset, 4 firstInstance.
layout(set = 0, binding = 10, std430) restrict buffer CommandBuffer {
    uint command[];
} command_buffer;

layout(set = 0, binding = 11, std140) uniform ScatterParams {
    vec4 anchor_radius;       // xyz anchor dir, w planet radius
    vec4 right_spacing;      // xyz anchor right, w candidate spacing
    vec4 north_density;      // xyz anchor north, w density multiplier
    vec4 origin_geomorph;    // xyz floating origin, w geomorph spacing
    vec4 cell_grid_family;   // xy center cell, z grid dimension, w family
    vec4 texture_seed;       // x macro face res, y context face res, z scatter seed, w detail seed
    vec4 counts_bias_slope;  // x candidate count, y surface bias, z slope sample step, w enabled
    vec4 grass_shape;        // x min height, y max height, z width, w unused
} params;

const float PI = 3.14159265358979323846;
const float TAU = 6.28318530717958647692;

uint gm_hash(ivec3 p, uint seed) {
    uint h = uint(p.x) * 0x8da6b343u;
    h ^= uint(p.y) * 0xd8163841u;
    h ^= uint(p.z) * 0xcb1ab31fu;
    h ^= seed * 0x9e3779b9u;
    h ^= h >> uint(16);
    h *= 0x7feb352du;
    h ^= h >> uint(15);
    h *= 0x846ca68bu;
    h ^= h >> uint(16);
    return h;
}

float gm_rand(ivec3 p, uint seed) {
    return float(gm_hash(p, seed) & 0x00ffffffu) / 16777215.0;
}

float gm_value(vec3 p, uint seed) {
    ivec3 i = ivec3(floor(p));
    vec3 f = fract(p);
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = mix(gm_rand(i + ivec3(0,0,0), seed), gm_rand(i + ivec3(1,0,0), seed), f.x);
    float b = mix(gm_rand(i + ivec3(0,1,0), seed), gm_rand(i + ivec3(1,1,0), seed), f.x);
    float c = mix(gm_rand(i + ivec3(0,0,1), seed), gm_rand(i + ivec3(1,0,1), seed), f.x);
    float d = mix(gm_rand(i + ivec3(0,1,1), seed), gm_rand(i + ivec3(1,1,1), seed), f.x);
    return mix(mix(a, b, f.y), mix(c, d, f.y), f.z) * 2.0 - 1.0;
}

float gm_fbm(vec3 p, uint seed) {
    float h = 0.0;
    float a = 0.52;
    for (int i = 0; i < 5; i++) {
        h += gm_value(p, seed + uint(i * 17)) * a;
        p = p * 2.01 + vec3(17.0, -11.0, 7.0);
        a *= 0.48;
    }
    return h;
}

float gm_ridged(vec3 p, uint seed) {
    return 1.0 - abs(gm_fbm(p, seed));
}

vec3 gm_domain_warp(vec3 p, uint seed, float strength) {
    vec3 w = vec3(
        gm_fbm(p + vec3(13.1, 7.7, -4.3), seed + 101u),
        gm_fbm(p + vec3(-5.7, 19.3, 8.9), seed + 211u),
        gm_fbm(p + vec3(9.2, -3.8, 23.4), seed + 307u));
    return p + w * strength;
}

float gm_cellular_ridge(vec3 p, uint seed) {
    ivec3 base = ivec3(floor(p));
    vec3 f = fract(p);
    ivec3 side = ivec3(f.x < 0.5 ? -1 : 1, f.y < 0.5 ? -1 : 1, f.z < 0.5 ? -1 : 1);
    float f1 = 1e9;
    float f2 = 1e9;
    for (int z = 0; z <= 1; z++) {
        for (int y = 0; y <= 1; y++) {
            for (int x = 0; x <= 1; x++) {
                ivec3 c = base + ivec3(x * side.x, y * side.y, z * side.z);
                vec3 jitter = vec3(gm_rand(c, seed + 3u), gm_rand(c, seed + 5u), gm_rand(c, seed + 7u));
                vec3 feature = vec3(c) + vec3(0.25) + jitter * 0.50;
                float d2 = dot(p - feature, p - feature);
                if (d2 < f1) {
                    f2 = f1;
                    f1 = d2;
                } else if (d2 < f2) {
                    f2 = d2;
                }
            }
        }
    }
    return clamp(1.0 - (f2 - f1) * 2.8, 0.0, 1.0);
}

vec3 surface_face_uv(vec3 dir) {
    vec3 d = normalize(dir);
    vec3 ad = abs(d);
    int face = 0;
    vec3 axis = vec3(1.0, 0.0, 0.0);
    vec3 right = vec3(0.0, 0.0, -1.0);
    vec3 up = vec3(0.0, 1.0, 0.0);
    if (ad.x >= ad.y && ad.x >= ad.z) {
        if (d.x >= 0.0) {
            face = 0; axis = vec3(1,0,0); right = vec3(0,0,-1); up = vec3(0,1,0);
        } else {
            face = 1; axis = vec3(-1,0,0); right = vec3(0,0,1); up = vec3(0,1,0);
        }
    } else if (ad.y >= ad.z) {
        if (d.y >= 0.0) {
            face = 2; axis = vec3(0,1,0); right = vec3(-1,0,0); up = vec3(0,0,1);
        } else {
            face = 3; axis = vec3(0,-1,0); right = vec3(-1,0,0); up = vec3(0,0,-1);
        }
    } else {
        if (d.z >= 0.0) {
            face = 4; axis = vec3(0,0,1); right = vec3(1,0,0); up = vec3(0,1,0);
        } else {
            face = 5; axis = vec3(0,0,-1); right = vec3(-1,0,0); up = vec3(0,1,0);
        }
    }
    float denom = max(dot(axis, d), 1e-8);
    float q = PI * 0.25;
    float u = atan(dot(right, d) / denom) / q;
    float v = atan(dot(up, d) / denom) / q;
    return vec3(u * 0.5 + 0.5, v * 0.5 + 0.5, float(face));
}

vec3 array_coord(vec3 dir, float face_res, sampler2DArray tex) {
    vec3 fuv = surface_face_uv(dir);
    ivec3 size3 = textureSize(tex, 0);
    float tex_res = float(size3.x);
    float gutter = max((tex_res - face_res) * 0.5, 0.0);
    vec2 uv = (fuv.xy * face_res + vec2(gutter)) / max(tex_res, 1.0);
    return vec3(uv, fuv.z);
}

vec4 sample_linear(sampler2DArray tex, vec3 dir, float face_res) {
    vec3 coord = array_coord(dir, face_res, tex);
    ivec3 size3 = textureSize(tex, 0);
    vec2 p = coord.xy * vec2(size3.xy) - vec2(0.5);
    ivec2 p0 = ivec2(floor(p));
    vec2 f = fract(p);
    ivec2 hi = size3.xy - ivec2(1);
    ivec2 a = clamp(p0, ivec2(0), hi);
    ivec2 b = clamp(p0 + ivec2(1,0), ivec2(0), hi);
    ivec2 c = clamp(p0 + ivec2(0,1), ivec2(0), hi);
    ivec2 e = clamp(p0 + ivec2(1,1), ivec2(0), hi);
    int layer = int(coord.z + 0.5);
    vec4 v00 = texelFetch(tex, ivec3(a, layer), 0);
    vec4 v10 = texelFetch(tex, ivec3(b, layer), 0);
    vec4 v01 = texelFetch(tex, ivec3(c, layer), 0);
    vec4 v11 = texelFetch(tex, ivec3(e, layer), 0);
    return mix(mix(v00, v10, f.x), mix(v01, v11, f.x), f.y);
}

float sample_nearest_r(sampler2DArray tex, vec3 dir, float face_res) {
    vec3 coord = array_coord(dir, face_res, tex);
    ivec3 size3 = textureSize(tex, 0);
    ivec2 p = clamp(ivec2(floor(coord.xy * vec2(size3.xy))), ivec2(0), size3.xy - ivec2(1));
    return texelFetch(tex, ivec3(p, int(coord.z + 0.5)), 0).r;
}

float macro_height(vec3 dir) {
    return sample_linear(macro_tex, dir, params.texture_seed.x).r;
}

vec4 ctx_soil(vec3 dir) { return sample_linear(soil_tex, dir, params.texture_seed.y); }
vec4 ctx_surface(vec3 dir) { return sample_linear(surface_tex, dir, params.texture_seed.y); }
vec4 ctx_geology(vec3 dir) { return sample_linear(geology_tex, dir, params.texture_seed.y); }
vec4 ctx_structure(vec3 dir) { return sample_linear(structure_tex, dir, params.texture_seed.y); }
vec4 ctx_climate(vec3 dir) { return sample_linear(climate_tex, dir, params.texture_seed.y); }
vec4 ctx_hydro(vec3 dir) { return sample_linear(hydrology_tex, dir, params.texture_seed.y); }
float ctx_rock_id(vec3 dir) { return floor(sample_nearest_r(rock_tex, dir, params.texture_seed.y) * 12.0 + 0.5); }
float ctx_biome_id(vec3 dir) { return floor(sample_nearest_r(biome_tex, dir, params.texture_seed.y) * 19.0 + 0.5); }
float ctx_soil_depth(vec4 surface) { return surface.r * 14.0; }
float ctx_temp(vec4 climate) { return mix(-40.0, 50.0, climate.r); }
float ctx_precip(vec4 climate) { return climate.g * 3000.0; }
float ctx_uplift(vec4 structure) { return structure.r * 2.0 - 1.0; }
vec2 ctx_flow(vec4 hydro) { return hydro.rg * 2.0 - 1.0; }

vec4 landform_weights(float macro_h, vec4 soil, vec4 surface, vec4 geology,
        vec4 structure, vec4 climate, vec4 hydro) {
    float uplift = max(ctx_uplift(structure), 0.0);
    float fault = structure.g;
    float plate_boundary = geology.a;
    float floodplain = structure.b;
    float wetland = structure.a;
    float moisture = surface.g;
    float vegetation = surface.b;
    float sand = soil.r;
    float erodible = geology.r;
    float temp_c = ctx_temp(climate);
    float precip = ctx_precip(climate);
    float altitude = smoothstep(450.0, 2600.0, max(macro_h, 0.0));
    float tectonic = clamp(uplift * 0.82 + fault * 0.50 + plate_boundary * 0.28, 0.0, 1.0);
    float mountain = clamp(max(tectonic, altitude * 0.50), 0.0, 1.0);
    float arid = clamp((1.0 - moisture) * (1.0 - clamp(precip / 900.0, 0.0, 1.0))
        * (0.35 + sand * 0.85) * (1.0 - vegetation * 0.55), 0.0, 1.0);
    float cold = 1.0 - smoothstep(-12.0, 4.0, temp_c);
    float glacial = clamp(cold * (0.35 + altitude * 0.75), 0.0, 1.0);
    float depositional = clamp(max(hydro.a, floodplain * 0.85 + wetland * 0.9)
        * (0.5 + erodible * 0.35), 0.0, 1.0);
    return vec4(mountain, arid, glacial, depositional);
}

float band_weight(float wavelength_m, float spacing_m) {
    float x = clamp((wavelength_m - spacing_m * 4.0) / max(spacing_m * 4.0, 1e-5), 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

void tangent_basis(vec3 dir, out vec3 right, out vec3 north) {
    vec3 basis_up = abs(dir.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    right = normalize(cross(basis_up, dir));
    north = normalize(cross(dir, right));
}

vec3 flow_world(vec3 dir, vec2 flow_xy) {
    vec3 right, north;
    tangent_basis(dir, right, north);
    vec3 flow = right * flow_xy.x + north * flow_xy.y;
    return dot(flow, flow) > 1e-5 ? normalize(flow) : vec3(0.0);
}

float geomorph_height(vec3 dir, float macro_h) {
    float radius = params.anchor_radius.w;
    float spacing_m = params.origin_geomorph.w;
    uint seed = uint(max(params.texture_seed.w, 1.0));
    vec3 world_m = dir * radius;
    vec4 soil = ctx_soil(dir);
    vec4 surface = ctx_surface(dir);
    vec4 geology = ctx_geology(dir);
    vec4 structure = ctx_structure(dir);
    vec4 climate = ctx_climate(dir);
    vec4 hydro = ctx_hydro(dir);
    vec4 weights = landform_weights(macro_h, soil, surface, geology, structure, climate, hydro);
    float mountain = weights.r;
    float arid = weights.g;
    float glacial = weights.b;
    float depositional = weights.a;
    float hardness = 1.20 - geology.r * 0.48;
    float h = 0.0;

    float w16k = band_weight(16000.0, spacing_m);
    if (w16k > 0.001) {
        vec3 p = gm_domain_warp(world_m / 16000.0, seed + 11u, 0.8);
        h += gm_fbm(p, seed + 13u) * mix(24.0, 125.0, mountain) * w16k;
    }
    float w6k = band_weight(6000.0, spacing_m);
    if (w6k > 0.001 && mountain > 0.08) {
        vec3 p = gm_domain_warp(world_m / 6000.0, seed + 31u, 1.1);
        float cells = gm_cellular_ridge(p, seed + 37u);
        float ridge = gm_ridged(p * 1.55, seed + 41u);
        float skeleton = mix(ridge, cells, 0.58);
        h += (skeleton * 2.0 - 1.0) * 210.0 * mountain * hardness * w6k;
    }
    float w1400 = band_weight(1400.0, spacing_m);
    if (w1400 > 0.001) {
        vec3 p = gm_domain_warp(world_m / 1400.0, seed + 53u, 0.72);
        float ridge = gm_ridged(p * 1.25, seed + 59u);
        float detail = gm_fbm(p * 2.1, seed + 61u);
        h += ((ridge * 2.0 - 1.0) * 72.0 * mountain + detail * 24.0) * w1400;
    }
    float w420 = band_weight(420.0, spacing_m);
    if (w420 > 0.001) {
        vec3 flow = flow_world(dir, ctx_flow(hydro));
        vec3 sample_m = world_m;
        if (dot(flow, flow) > 0.1) {
            float along_q = dot(sample_m, flow);
            vec3 along = flow * along_q;
            vec3 across = sample_m - along;
            sample_m = along * 0.42 + across * 1.45;
        }
        vec3 p = gm_domain_warp(sample_m / 420.0, seed + 71u, 0.55);
        float channel = pow(gm_ridged(p, seed + 73u), 4.6);
        h -= channel * mix(2.0, 34.0, hydro.b) * (1.0 - depositional * 0.78) * w420;
        float fan = pow(gm_ridged(p * 0.48 + vec3(9.0), seed + 79u), 2.2);
        h += fan * depositional * mix(1.0, 12.0, surface.a) * w420;
    }
    float w120 = band_weight(120.0, spacing_m);
    if (w120 > 0.001) {
        float n = gm_fbm(world_m / 120.0, seed + 89u);
        float dune = gm_ridged(gm_domain_warp(world_m / 180.0, seed + 97u, 0.45), seed + 101u);
        h += n * 4.5 * (1.0 - glacial * 0.6) * w120;
        h += (dune * 2.0 - 1.0) * 9.0 * arid * soil.r * w120;
    }
    float w24 = band_weight(24.0, spacing_m);
    if (w24 > 0.001) h += gm_value(world_m / 24.0, seed + 109u) * 0.9 * w24;
    if (glacial > 0.01) {
        float ice = gm_fbm(world_m / 2600.0 + vec3(2.0, -7.0, 5.0), seed + 127u);
        h = mix(h, h * 0.62 + ice * 52.0, glacial * 0.72);
    }
    return h;
}

float terrain_height(vec3 dir) {
    float macro_h = macro_height(dir);
    return macro_h + geomorph_height(dir, macro_h);
}

float is_biome(float biome, float id) {
    return 1.0 - step(0.49, abs(biome - id));
}

vec4 primary_weights(vec4 soil, vec4 surface, vec4 geology, vec4 structure,
        vec4 climate, vec4 hydro, vec4 landform, float biome, float slope_deg) {
    float soil_depth = ctx_soil_depth(surface);
    float wet = surface.g;
    float veg_potential = surface.b;
    float sediment = surface.a;
    float mountain = landform.r;
    float arid = landform.g;
    float soil_repose = mix(42.0, 27.0, clamp(wet * 0.75 + soil.b * 0.45, 0.0, 1.0));
    float loose_stable = 1.0 - smoothstep(soil_repose - 4.0, soil_repose + 5.0, slope_deg);
    float sand_stable = 1.0 - smoothstep(29.0, 36.0, slope_deg);
    float veg_stable = 1.0 - smoothstep(32.0, 46.0, slope_deg);
    float thin = 1.0 - smoothstep(0.06, 0.55, soil_depth);
    float forced_rock = max(is_biome(biome, 18.0), smoothstep(43.0, 58.0, slope_deg));
    float rock = clamp(max(thin * (1.0 - veg_potential * 0.65), forced_rock), 0.0, 1.0);
    rock = max(rock, mountain * smoothstep(48.0, 62.0, slope_deg));
    float sand = soil.r * arid * sand_stable * (1.0 - rock);
    sand *= 0.45 + sediment * 0.85;
    if (is_biome(biome, 12.0) > 0.5) sand = max(sand, soil.r * 0.72 * sand_stable);
    float vegetation = veg_potential * veg_stable * loose_stable * (1.0 - rock);
    vegetation *= 1.0 - arid * 0.62;
    vegetation *= smoothstep(0.04, 0.28, soil_depth);
    float mineral_soil = loose_stable * (1.0 - rock) * (1.0 - sand * 0.72);
    mineral_soil *= smoothstep(0.025, 0.22, soil_depth);
    return clamp(vec4(rock, mineral_soil, vegetation, sand), 0.0, 1.0);
}

vec4 secondary_weights(vec4 soil, vec4 surface, vec4 geology, vec4 structure,
        vec4 climate, vec4 hydro, vec4 landform, float biome, float slope_deg) {
    float wet = surface.g;
    float sediment = surface.a;
    float mountain = landform.r;
    float depositional = landform.a;
    float temp = ctx_temp(climate);
    float mud = wet * (soil.g * 0.55 + soil.b * 0.85) * (1.0 - smoothstep(12.0, 28.0, slope_deg));
    mud *= 0.35 + depositional * 0.95;
    mud = max(mud, is_biome(biome, 16.0) * wet * 0.8);
    float cold = 1.0 - smoothstep(-8.0, 2.0, temp);
    float snow = cold * (1.0 - smoothstep(28.0, 50.0, slope_deg));
    snow = max(snow, is_biome(biome, 3.0));
    float alpine_cold = 1.0 - smoothstep(-5.0, 3.0, temp);
    snow = max(snow, is_biome(biome, 17.0) * alpine_cold);
    float scree = mountain * smoothstep(25.0, 34.0, slope_deg)
        * (1.0 - smoothstep(45.0, 55.0, slope_deg));
    scree *= clamp(0.35 + sediment * 0.9 + geology.r * 0.35, 0.0, 1.0);
    float gravel = hydro.b * (1.0 - smoothstep(18.0, 34.0, slope_deg));
    gravel *= clamp(0.35 + sediment * 0.9, 0.0, 1.0);
    gravel = max(gravel, is_biome(biome, 19.0) * 0.72);
    return clamp(vec4(mud, snow, scree, gravel), 0.0, 1.0);
}

vec3 vegetation_color(vec4 surface, vec4 climate, float biome) {
    float wet = surface.g;
    float cold = clamp((5.0 - ctx_temp(climate)) / 18.0, 0.0, 1.0);
    vec3 green = mix(vec3(0.14, 0.19, 0.055), vec3(0.055, 0.145, 0.038), wet);
    green = mix(green, vec3(0.115, 0.120, 0.070), cold * 0.55);
    float steppe = is_biome(biome, 11.0);
    float savanna = is_biome(biome, 13.0);
    green = mix(green, vec3(0.205, 0.175, 0.070), max(steppe, savanna) * (1.0 - wet) * 0.75);
    return green;
}

float hash01(uint h) { return float(h & 0x00ffffffu) / 16777215.0; }
float hash_lane(uint h, uint shift) { return float((h >> shift) & 0x000003ffu) / 1023.0; }

ivec2 local_cell(int candidate) {
    int grid = int(params.cell_grid_family.z + 0.5);
    int gx = candidate % grid;
    int gy = candidate / grid;
    int half_grid = grid / 2;
    return ivec2(gx - half_grid, gy - half_grid);
}

float window_fade(ivec2 local, int grid) {
    vec2 q = vec2(local) / max(float(grid) * 0.5, 1.0);
    return 1.0 - smoothstep(0.86, 1.0, length(q));
}

vec3 candidate_direction(ivec2 absolute_cell, uint h) {
    vec2 jitter = (vec2(hash_lane(h, 0u), hash_lane(h, 10u)) - vec2(0.5)) * (params.right_spacing.w * 0.72);
    vec2 offset_m = vec2(absolute_cell) * params.right_spacing.w + jitter;
    return normalize(params.anchor_radius.xyz
        + params.right_spacing.xyz * (offset_m.x / max(params.anchor_radius.w, 1.0))
        + params.north_density.xyz * (offset_m.y / max(params.anchor_radius.w, 1.0)));
}

float exact_slope_deg(vec3 dir, float center_h, out vec3 normal) {
    vec3 right, north;
    tangent_basis(dir, right, north);
    float eps_m = max(params.counts_bias_slope.z, 0.35);
    float inv_r = 1.0 / max(params.anchor_radius.w, 1.0);
    vec3 dx = normalize(dir + right * (eps_m * inv_r));
    vec3 dy = normalize(dir + north * (eps_m * inv_r));
    float hx = terrain_height(dx);
    float hy = terrain_height(dy);
    vec3 p0 = dir * (params.anchor_radius.w + center_h);
    vec3 px = dx * (params.anchor_radius.w + hx);
    vec3 py = dy * (params.anchor_radius.w + hy);
    normal = normalize(cross(px - p0, py - p0));
    if (dot(normal, dir) < 0.0) normal = -normal;
    return degrees(acos(clamp(dot(normal, dir), 0.0, 1.0)));
}

float suitability_for_family(int family, float terrain_h, float slope_deg,
        vec4 soil, vec4 surface, vec4 geology, vec4 structure, vec4 climate,
        vec4 hydro, vec4 landform, float biome) {
    vec4 primary = primary_weights(soil, surface, geology, structure, climate, hydro, landform, biome, slope_deg);
    vec4 secondary = secondary_weights(soil, surface, geology, structure, climate, hydro, landform, biome, slope_deg);
    if (family == 0) {
        float land = smoothstep(-0.35, 1.5, terrain_h);
        float wetland_penalty = mix(1.0, 0.48, structure.a);
        float mud_penalty = 1.0 - secondary.r * 0.72;
        float snow_penalty = 1.0 - secondary.g;
        float bare_penalty = 1.0 - is_biome(biome, 18.0);
        float ice_penalty = 1.0 - is_biome(biome, 3.0);
        return clamp(primary.b * wetland_penalty * mud_penalty * snow_penalty
            * bare_penalty * ice_penalty * land, 0.0, 1.0);
    }
    if (family == 1) {
        float soil_depth = ctx_soil_depth(surface);
        float thin = 1.0 - smoothstep(0.10, 0.85, soil_depth);
        float exposure = max(primary.r * 0.72, secondary.b * 1.10);
        exposure = max(exposure, landform.r * thin * (0.38 + geology.r * 0.52));
        exposure *= 1.0 - secondary.g * 0.88;
        exposure *= smoothstep(-0.25, 1.0, terrain_h);
        return clamp(exposure, 0.0, 1.0);
    }
    float channel = hydro.b * (0.35 + hydro.a * 0.95);
    float gravel = max(secondary.a, channel * (0.35 + surface.a * 0.65));
    float mud_penalty = 1.0 - secondary.r * 0.70;
    float land = smoothstep(-0.50, 0.70, terrain_h);
    return clamp(gravel * mud_penalty * land, 0.0, 1.0);
}

float cheap_upper_bound(int family, vec4 soil, vec4 surface, vec4 geology,
        vec4 structure, vec4 climate, vec4 hydro, vec4 landform, float biome) {
    if (family == 0) {
        return clamp(surface.b * (1.0 - landform.g * 0.62), 0.0, 1.0);
    }
    if (family == 2) {
        float a = hydro.b * clamp(0.35 + surface.a * 0.90, 0.0, 1.25);
        float b = hydro.b * (0.35 + hydro.a * 0.95) * (0.35 + surface.a * 0.65);
        return clamp(max(max(a, b), is_biome(biome, 19.0) * 0.72), 0.0, 1.0);
    }
    // Geological rock exposure depends strongly on the exact local slope. Keep
    // the bound conservative so isolated procedural cliffs are never lost.
    return 1.0;
}

void write_instance(uint dst, vec3 x_axis, vec3 y_axis, vec3 z_axis,
        vec3 origin, vec4 custom) {
    uint base = dst * 4u;
    instance_buffer.data[base + 0u] = vec4(x_axis.x, y_axis.x, z_axis.x, origin.x);
    instance_buffer.data[base + 1u] = vec4(x_axis.y, y_axis.y, z_axis.y, origin.y);
    instance_buffer.data[base + 2u] = vec4(x_axis.z, y_axis.z, z_axis.z, origin.z);
    instance_buffer.data[base + 3u] = custom;
}

void main() {
    uint candidate_u = gl_GlobalInvocationID.x;
    int candidate_count = int(params.counts_bias_slope.x + 0.5);
    if (candidate_u >= uint(candidate_count) || params.counts_bias_slope.w < 0.5) return;

    int candidate = int(candidate_u);
    int grid = int(params.cell_grid_family.z + 0.5);
    int family = int(params.cell_grid_family.w + 0.5);
    ivec2 lc = local_cell(candidate);
    float edge = window_fade(lc, grid);
    if (edge <= 0.001) return;

    ivec2 center = ivec2(round(params.cell_grid_family.xy));
    ivec2 absolute_cell = center + lc;
    uint salt = family == 0 ? 0x117u : (family == 1 ? 0x2b7u : 0x5d1u);
    uint scatter_seed = uint(max(params.texture_seed.z, 1.0));
    uint h = gm_hash(ivec3(absolute_cell.x, absolute_cell.y, int(salt & 0x7fffu)), scatter_seed + salt * 131u);
    float decision = hash01(gm_hash(ivec3(int(h & 0xffffu), candidate, int(scatter_seed)), h ^ 0x9e37u));

    vec3 dir = candidate_direction(absolute_cell, h);
    float macro_h = macro_height(dir);
    vec4 soil = ctx_soil(dir);
    vec4 surface = ctx_surface(dir);
    vec4 geology = ctx_geology(dir);
    vec4 structure = ctx_structure(dir);
    vec4 climate = ctx_climate(dir);
    vec4 hydro = ctx_hydro(dir);
    float biome = ctx_biome_id(dir);
    vec4 landform = landform_weights(macro_h, soil, surface, geology, structure, climate, hydro);

    float upper = cheap_upper_bound(family, soil, surface, geology, structure, climate, hydro, landform, biome)
        * params.north_density.w * edge;
    if (decision >= clamp(upper, 0.0, 0.995)) return;

    float terrain_h = macro_h + geomorph_height(dir, macro_h);
    vec3 normal;
    float slope_deg = exact_slope_deg(dir, terrain_h, normal);
    float suitability = suitability_for_family(family, terrain_h, slope_deg,
        soil, surface, geology, structure, climate, hydro, landform, biome);
    float threshold = clamp(suitability * params.north_density.w * edge, 0.0, 0.985);
    if (decision >= threshold) return;

    uint dst = atomicAdd(command_buffer.command[1], 1u);
    if (dst >= uint(candidate_count)) return;

    vec3 tangent_right, tangent_north;
    tangent_basis(dir, tangent_right, tangent_north);
    tangent_right = normalize(tangent_right - normal * dot(tangent_right, normal));
    tangent_north = normalize(cross(normal, tangent_right));

    float r0 = hash_lane(h, 0u);
    float r1 = hash_lane(h, 10u);
    float r2 = hash_lane(h, 20u);
    float angle = r0 * TAU;
    float ca = cos(angle);
    float sa = sin(angle);
    vec3 yaw_x = tangent_right * ca + tangent_north * sa;
    vec3 yaw_z = -tangent_right * sa + tangent_north * ca;
    vec3 local_origin = dir * (params.anchor_radius.w + terrain_h + params.counts_bias_slope.y)
        - params.origin_geomorph.xyz;

    if (family == 0) {
        float biome_short = max(is_biome(biome, 17.0), is_biome(biome, 11.0));
        float height_m = mix(params.grass_shape.x, params.grass_shape.y, 0.25 + r1 * 0.75);
        height_m *= mix(1.0, 0.62, biome_short);
        float width_m = params.grass_shape.z * mix(0.72, 1.28, r2);
        vec3 base_color = vegetation_color(surface, climate, biome);
        float variation = mix(0.82, 1.15, hash01(h ^ 0x4ad3u));
        vec3 color = clamp(base_color * variation, vec3(0.018), vec3(0.42));
        write_instance(dst, yaw_x * width_m, normal * height_m, yaw_z * width_m,
            local_origin, vec4(color, hash01(h ^ 0xa52du)));
        return;
    }

    vec3 scale_xyz;
    if (family == 1) {
        float base_scale = mix(0.16, 0.92, r1 * r1);
        float boulder_boost = smoothstep(0.83, 0.99, r2) * landform.r * 0.85;
        base_scale *= 1.0 + boulder_boost;
        scale_xyz = vec3(base_scale * mix(0.72, 1.22, r0),
            base_scale * mix(0.62, 1.18, r2), base_scale * mix(0.76, 1.28, r1));
    } else {
        float base_scale = mix(0.075, 0.34, r1 * r1);
        scale_xyz = vec3(base_scale * mix(0.90, 1.35, r0),
            base_scale * mix(0.32, 0.62, r2), base_scale * mix(0.88, 1.32, r1));
    }
    float rock_id = ctx_rock_id(dir);
    write_instance(dst, yaw_x * scale_xyz.x, normal * scale_xyz.y, yaw_z * scale_xyz.z,
        local_origin, vec4(rock_id / 12.0, geology.r, surface.g, hash01(h ^ 0x6f21u)));
}
