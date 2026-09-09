#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform writeonly image3D light_volume;
layout(set = 0, binding = 1) uniform sampler3D shape_noise;
layout(set = 0, binding = 2) uniform sampler2D global_weather;
layout(set = 0, binding = 3) uniform sampler2D local_weather;

layout(push_constant, std430) uniform Params {
    // xyz local weather centre unit vector, w local tangent span in metres.
    vec4 center_span;
    // xyz Helion direction, w irradiance scale.
    vec4 sun_intensity;
    // xyz cloud noise wind offset, w planet radius.
    vec4 wind_radius;
    // xyz texture dimensions, w current time-slice phase [0,11].
    vec4 resolution_phase;
} params;

const float PI = 3.14159265358979323846;
const float CLOUD_TOP = 14500.0;
const float CLOUD_SHAPE_SCALE = 0.0000520;
const float CLOUD_EXTINCTION = 0.0010;
const float WORLEY_PERSISTENCE = 0.57;
const int LIGHT_STEPS = 4;
const int TIME_SLICES = 12;

vec2 global_uv(vec3 d) {
    d = normalize(d);
    float lon = atan(d.z, d.x);
    if (lon < 0.0) lon += 2.0 * PI;
    float lat = asin(clamp(d.y, -1.0, 1.0));
    return vec2(lon / (2.0 * PI), (PI * 0.5 - lat) / PI);
}

void local_basis(out vec3 center, out vec3 east, out vec3 north) {
    center = normalize(params.center_span.xyz);
    vec3 pole = abs(center.y) > 0.92 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    east = normalize(cross(pole, center));
    north = normalize(cross(center, east));
}

vec4 weather_at(vec3 direction, float radius) {
    vec4 g = textureLod(global_weather, global_uv(direction), 0.0);
    vec3 center, east, north;
    local_basis(center, east, north);
    vec3 delta = normalize(direction) * radius - center * radius;
    float span = max(params.center_span.w, 1000.0);
    vec2 uv = vec2(dot(delta, east), dot(delta, north)) / span + vec2(0.5);
    float edge = max(abs(uv.x - 0.5), abs(uv.y - 0.5));
    float blend = 1.0 - smoothstep(0.42, 0.50, edge);
    vec4 l = textureLod(local_weather, clamp(uv, vec2(0.0), vec2(1.0)), 0.0);
    return mix(g, l, blend);
}

float worley(vec3 uv) {
    return 1.0 - textureLod(shape_noise, uv, 0.0).r;
}

float worley_fbm(vec3 p, vec3 wind) {
    vec3 uv = (p + wind) * CLOUD_SHAPE_SCALE;
    float n0 = worley(uv);
    float n1 = worley(uv * 2.03 + vec3(0.19, 0.61, 0.43));
    return (n0 + WORLEY_PERSISTENCE * n1) / (1.0 + WORLEY_PERSISTENCE);
}

float vertical_profile(float altitude, float storm) {
    float base_alt = mix(1100.0, 900.0, storm);
    float top_alt = mix(4200.0, CLOUD_TOP, smoothstep(0.25, 0.85, storm));
    if (altitude <= base_alt || altitude >= top_alt) return 0.0;
    float bottom = smoothstep(base_alt, base_alt + 220.0, altitude);
    float top = 1.0 - smoothstep(top_alt - mix(750.0, 1500.0, storm), top_alt, altitude);
    float anvil = smoothstep(0.50, 0.78, storm)
        * exp(-pow((altitude - 11250.0) / 2200.0, 2.0));
    return max(bottom * top, anvil * 0.72);
}

float coarse_density(vec3 p, float radius, vec3 wind) {
    float altitude = length(p) - radius;
    if (altitude <= 0.0 || altitude >= CLOUD_TOP) return 0.0;
    vec4 wx = weather_at(normalize(p), radius);
    float coverage = clamp(wx.r, 0.0, 1.0);
    float storm = clamp(wx.g, 0.0, 1.0);
    float precip = clamp(wx.b, 0.0, 1.0);
    float lowp = clamp((0.5 - wx.a) * 3.0, 0.0, 1.0);
    float conv = clamp(max(storm, precip * 0.72 + lowp * 0.18), 0.0, 1.0);
    float effective = mix(smoothstep(0.04, 0.72, coverage),
        smoothstep(0.02, 0.55, coverage), conv);
    float threshold = 1.0 - effective * 0.76;
    float macro = worley_fbm(p, wind);
    float hardness = mix(0.11, 0.045, conv);
    float body = smoothstep(threshold, min(threshold + hardness, 0.999), macro);
    return body * vertical_profile(altitude, conv) * (0.55 + 0.75 * conv);
}

float sun_transmittance(vec3 p, float radius, vec3 sun_dir, vec3 wind) {
    float step_len = 2000.0 / float(LIGHT_STEPS);
    float optical_depth = 0.0;
    for (int i = 0; i < LIGHT_STEPS; ++i) {
        float f = (float(i) + 0.55) / float(LIGHT_STEPS);
        float shaped = mix(f, f * f, 0.65);
        optical_depth += coarse_density(p + sun_dir * shaped * 2000.0,
            radius, wind) * step_len;
    }
    return exp(-optical_depth * CLOUD_EXTINCTION * 0.90);
}

void main() {
    ivec2 xy = ivec2(gl_GlobalInvocationID.xy);
    ivec3 size3 = imageSize(light_volume);
    if (xy.x >= size3.x || xy.y >= size3.y) return;

    int phase = int(params.resolution_phase.w + 0.5) % TIME_SLICES;
    float radius = params.wind_radius.w;
    vec3 wind = params.wind_radius.xyz;
    vec3 sun_dir = normalize(params.sun_intensity.xyz);
    vec3 center, east, north;
    local_basis(center, east, north);
    float span = max(params.center_span.w, 1000.0);

    vec2 q = (vec2(xy) + vec2(0.5)) / vec2(size3.xy) - vec2(0.5);
    vec3 surface_dir = normalize(center
        + east * (q.x * span / radius)
        + north * (q.y * span / radius));

    for (int z = phase; z < size3.z; z += TIME_SLICES) {
        float altitude = (float(z) + 0.5) / float(size3.z) * CLOUD_TOP;
        vec3 p = surface_dir * (radius + altitude);
        float density = coarse_density(p, radius, wind);
        float direct_t = density > 0.002
            ? sun_transmittance(p, radius, sun_dir, wind)
            : 1.0;

        // EVE's volume primarily amortises indirect cloud lighting. R keeps a
        // slowly varying direct term; G stores the multiple-scatter reservoir;
        // B stores sky fill and A the coarse density used for confidence/weight.
        float trapped = (1.0 - direct_t) * density;
        float multiple = (0.020 + trapped * 0.13) * (0.35 + 0.65 * density);
        float skylight = mix(0.065, 0.025, density) + trapped * 0.025;
        imageStore(light_volume, ivec3(xy, z),
            vec4(direct_t, multiple, skylight, density));
    }
}
