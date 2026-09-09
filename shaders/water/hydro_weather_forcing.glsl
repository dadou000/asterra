#[compute]
#version 450

// Converts the published global weather precipitation texture into the sparse SWE
// atmospheric-source layer. Only resident hydrology cells are written.
//
// WeatherSystem global weather RGBA contract:
//   B = normalized instantaneous precipitation intensity [0..1]
//       where 1.0 = 30 mm/h water equivalent in WeatherNative.
//
// Output source layout matches sparse_hydro_step.glsl:
//   x = add depth rate [m/s]
//   y = remove depth rate [m/s]
//   z = hu injection rate
//   w = hv injection rate
//
// Until a soil water bucket exists, land infiltration is represented as an
// interception capacity against *current rainfall* only. It never drains already
// ponded/lake water. Ocean/sub-sea cells receive the full precipitation rate.

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
layout(set = 0, binding = 3, std430) writeonly buffer AtmosphericSources {
    vec4 atmospheric_sources[];
};
layout(set = 0, binding = 4, std430) readonly buffer Params {
    vec4 grid;    // tile_res, capacity, precipitation_max_mps, infiltration_mps
    vec4 policy;  // weather_gain, land_bed_threshold_m, min_output_mps, reserved
} params;
layout(set = 0, binding = 5) uniform sampler2D weather_global;

const float PI = 3.14159265358979323846;
const float TAU = 6.28318530717958647692;
const float CUBE_Q = 0.78539816339744830962;

int tile_res() { return max(int(params.grid.x + 0.5), 1); }
int capacity() { return max(int(params.grid.y + 0.5), 1); }
int cells_per_tile() { int r = tile_res(); return r * r; }
int idx(int slot, ivec2 p) { return slot * cells_per_tile() + p.y * tile_res() + p.x; }

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

vec3 face_uv_to_dir(int face, vec2 uv) {
    // Same equiangular cube mapping used by the sparse reconstruction path.
    vec2 q = tan(uv * CUBE_Q);
    return normalize(face_axis(face) + face_right(face) * q.x + face_up(face) * q.y);
}

vec3 cell_direction(ivec4 key, ivec2 cell_xy) {
    int r = tile_res();
    int level = clamp(key.y, 0, 27);
    int side = 1 << level;
    vec2 global01 = (vec2(key.zw) + (vec2(cell_xy) + vec2(0.5)) / float(r))
        / float(side);
    vec2 uv = global01 * 2.0 - 1.0;
    return face_uv_to_dir(key.x, uv);
}

vec2 weather_uv(vec3 direction) {
    vec3 d = normalize(direction);
    float lon = atan(d.z, d.x);
    if (lon < 0.0) lon += TAU;
    float lat = asin(clamp(d.y, -1.0, 1.0));
    return vec2(lon / TAU, (0.5 * PI - lat) / PI);
}

void main() {
    ivec3 gid = ivec3(gl_GlobalInvocationID.xyz);
    int slot = gid.z;
    int r = tile_res();
    if (slot >= capacity() || gid.x >= r || gid.y >= r) return;
    ivec2 p = gid.xy;
    int i = idx(slot, p);

    if (occupied[slot] == 0) {
        atmospheric_sources[i] = vec4(0.0);
        return;
    }
    ivec4 key = tile_meta[slot];
    if (key.x < 0 || key.y < 0) {
        atmospheric_sources[i] = vec4(0.0);
        return;
    }

    vec3 d = cell_direction(key, p);
    float normalized_precip = clamp(texture(weather_global, weather_uv(d)).b, 0.0, 1.0);
    float rainfall = normalized_precip * max(params.grid.z, 0.0)
        * max(params.policy.x, 0.0);

    float bed = cells[i].w;
    bool land = bed > params.policy.y;
    float infiltration = land ? max(params.grid.w, 0.0) : 0.0;
    float runoff_input = max(rainfall - infiltration, 0.0);
    if (runoff_input < max(params.policy.z, 0.0)) runoff_input = 0.0;

    atmospheric_sources[i] = vec4(runoff_input, 0.0, 0.0, 0.0);
}
