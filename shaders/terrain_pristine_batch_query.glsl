#[compute]
#version 450

// Batched persistent-pristine terrain envelope query for Planet Studio.
//
// Unlike terrain_height_query.glsl this intentionally does NOT evaluate analytic
// geomorph/detail. Planet.global_height_texture already contains the authored
// runtime's macro elevation + coast-profile envelope. Persistent Smooth/Flatten/
// erosion tools can therefore query many envelope heights in one asynchronous GPU
// dispatch without baking high-frequency visible detail into sparse Deltas.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2DArray height_tex;
layout(set = 0, binding = 1, std430) restrict readonly buffer DirectionInput {
    vec4 value[];
} directions;
layout(set = 0, binding = 2, std430) restrict writeonly buffer HeightOutput {
    float value[];
} heights;
layout(set = 0, binding = 3, std140) uniform BatchParams {
    // x = logical cube-face resolution without gutters, y = query count.
    vec4 face_count;
} params;

const float PI = 3.14159265358979323846;

vec3 surface_face_uv(vec3 dir) {
    vec3 d = normalize(dir);
    vec3 ad = abs(d);
    int face = 0;
    vec3 axis = vec3(1.0, 0.0, 0.0);
    vec3 right = vec3(0.0, 0.0, -1.0);
    vec3 up = vec3(0.0, 1.0, 0.0);
    if (ad.x >= ad.y && ad.x >= ad.z) {
        if (d.x >= 0.0) {
            face = 0;
        } else {
            face = 1;
            axis = vec3(-1.0, 0.0, 0.0);
            right = vec3(0.0, 0.0, 1.0);
        }
    } else if (ad.y >= ad.z) {
        if (d.y >= 0.0) {
            face = 2;
            axis = vec3(0.0, 1.0, 0.0);
            right = vec3(-1.0, 0.0, 0.0);
            up = vec3(0.0, 0.0, 1.0);
        } else {
            face = 3;
            axis = vec3(0.0, -1.0, 0.0);
            right = vec3(-1.0, 0.0, 0.0);
            up = vec3(0.0, 0.0, -1.0);
        }
    } else {
        if (d.z >= 0.0) {
            face = 4;
            axis = vec3(0.0, 0.0, 1.0);
            right = vec3(1.0, 0.0, 0.0);
        } else {
            face = 5;
            axis = vec3(0.0, 0.0, -1.0);
            right = vec3(-1.0, 0.0, 0.0);
        }
    }
    float denom = max(dot(axis, d), 1e-8);
    float quarter_pi = PI * 0.25;
    return vec3(
        atan(dot(right, d) / denom) / quarter_pi * 0.5 + 0.5,
        atan(dot(up, d) / denom) / quarter_pi * 0.5 + 0.5,
        float(face)
    );
}

vec3 array_coord(vec3 dir, float face_res) {
    vec3 fuv = surface_face_uv(dir);
    ivec3 size_px = textureSize(height_tex, 0);
    float texture_res = float(size_px.x);
    float gutter = max((texture_res - face_res) * 0.5, 0.0);
    return vec3((fuv.xy * face_res + vec2(gutter)) / max(texture_res, 1.0), fuv.z);
}

vec4 cubic_weights(float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return vec4(
        (1.0 - 3.0 * t + 3.0 * t2 - t3) / 6.0,
        (4.0 - 6.0 * t2 + 3.0 * t3) / 6.0,
        (1.0 + 3.0 * t + 3.0 * t2 - 3.0 * t3) / 6.0,
        t3 / 6.0
    );
}

float pristine_envelope(vec3 dir) {
    vec3 coord = array_coord(dir, params.face_count.x);
    ivec3 tex_size_i = textureSize(height_tex, 0);
    vec2 tex_size = vec2(tex_size_i.xy);
    vec2 pixel = coord.xy * tex_size - vec2(0.5);
    vec2 base = floor(pixel);
    vec2 fraction = fract(pixel);
    vec4 wx = cubic_weights(fraction.x);
    vec4 wy = cubic_weights(fraction.y);
    float gx0 = wx.x + wx.y;
    float gx1 = wx.z + wx.w;
    float gy0 = wy.x + wy.y;
    float gy1 = wy.z + wy.w;
    float x0 = base.x - 1.0 + wx.y / max(gx0, 1e-9);
    float x1 = base.x + 1.0 + wx.w / max(gx1, 1e-9);
    float y0 = base.y - 1.0 + wy.y / max(gy0, 1e-9);
    float y1 = base.y + 1.0 + wy.w / max(gy1, 1e-9);
    vec2 uv00 = (vec2(x0, y0) + 0.5) / tex_size;
    vec2 uv10 = (vec2(x1, y0) + 0.5) / tex_size;
    vec2 uv01 = (vec2(x0, y1) + 0.5) / tex_size;
    vec2 uv11 = (vec2(x1, y1) + 0.5) / tex_size;
    float a = textureLod(height_tex, vec3(uv00, coord.z), 0.0).r;
    float b = textureLod(height_tex, vec3(uv10, coord.z), 0.0).r;
    float c = textureLod(height_tex, vec3(uv01, coord.z), 0.0).r;
    float d = textureLod(height_tex, vec3(uv11, coord.z), 0.0).r;
    return mix(mix(a, b, gx1), mix(c, d, gx1), gy1);
}

void main() {
    uint index = gl_GlobalInvocationID.x;
    uint count = uint(max(params.face_count.y, 0.0) + 0.5);
    if (index >= count) {
        return;
    }
    vec3 direction = normalize(directions.value[index].xyz);
    heights.value[index] = pristine_envelope(direction);
}
