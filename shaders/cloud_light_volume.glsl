#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(rgba16f, set = 0, binding = 0) uniform writeonly image3D light_volume;
layout(set = 0, binding = 1) uniform sampler3D shape_noise;
layout(set = 0, binding = 2) uniform sampler2D global_weather;
layout(set = 0, binding = 3) uniform sampler2D local_weather;

layout(push_constant, std430) uniform Params {
    vec4 center_span;
    vec4 sun_intensity;
    vec4 wind_radius;
    vec4 resolution_phase;
} params;

const float PI = 3.14159265358979323846;
const float CLOUD_TOP = 14500.0;
const float CLOUD_SHAPE_SCALE = 0.0000520;
const float CLOUD_EXTINCTION = 0.0010;
const float WORLEY_PERSISTENCE = 0.57;
const float LIGHT_DISTANCE = 16000.0;
const int LIGHT_STEPS = 10;
const int TIME_SLICES = 12;

float saturate1(float x) { return clamp(x, 0.0, 1.0); }

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

bool uv_inside_01(vec2 uv) {
    return all(greaterThanEqual(uv, vec2(0.0)))
        && all(lessThanEqual(uv, vec2(1.0)));
}

vec4 weather_at(vec3 direction, float radius) {
    direction = normalize(direction);
    vec4 g = textureLod(global_weather, global_uv(direction), 0.0);
    vec3 center, east, north;
    local_basis(center, east, north);
    vec3 delta = direction * radius - center * radius;
    float span = max(params.center_span.w, 1000.0);
    vec2 uv = vec2(dot(delta, east), dot(delta, north)) / span + vec2(0.5);
    if (!uv_inside_01(uv)) return g;
    float edge = max(abs(uv.x - 0.5), abs(uv.y - 0.5));
    float blend = 1.0 - smoothstep(0.40, 0.48, edge);
    vec4 l = textureLod(local_weather, uv, 0.0);
    return mix(g, l, blend);
}

float cloud_cell(vec3 uv) {
    return 1.0 - textureLod(shape_noise, uv, 0.0).r;
}

float cloud_fbm(vec3 p, float scale, vec3 advect) {
    vec3 uv = (p + advect) * scale;
    float n0 = cloud_cell(uv);
    float n1 = cloud_cell(uv * 2.03 + vec3(0.19, 0.61, 0.43));
    float n2 = cloud_cell(uv * 4.11 + vec3(0.73, 0.31, 0.11));
    float p1 = WORLEY_PERSISTENCE;
    float p2 = p1 * p1;
    return (n0 + p1 * n1 + p2 * n2) / (1.0 + p1 + p2);
}

vec3 cloud_domain_warp(vec3 surface_p, vec3 wind, float convection) {
    vec3 uv = (surface_p + wind * 0.18) * (CLOUD_SHAPE_SCALE * 0.16);
    vec3 q = vec3(
        cloud_cell(uv + vec3(0.13, 0.47, 0.81)),
        cloud_cell(uv + vec3(0.71, 0.23, 0.37)),
        cloud_cell(uv + vec3(0.41, 0.89, 0.17)));
    float metres = mix(720.0, 1650.0, convection);
    return (q * 2.0 - 1.0) * metres;
}

vec3 cloud_shear_offset(vec3 wind, float h, float convection) {
    float upper = h * h;
    vec3 cross_wind = vec3(-wind.z, wind.y * 0.12, wind.x);
    return wind * upper * mix(0.08, 0.28, convection)
        + cross_wind * upper * mix(0.025, 0.12, convection);
}

float canonical_coarse_density(vec3 p, float radius, vec3 wind) {
    float altitude = length(p) - radius;
    if (altitude <= 0.0 || altitude >= CLOUD_TOP) return 0.0;

    vec3 radial = normalize(p);
    vec3 surface_p = radial * radius;
    vec4 wx = weather_at(radial, radius);
    float coverage = saturate1(wx.r);
    float storm = saturate1(wx.g);
    float precip = saturate1(wx.b);
    float low_pressure = saturate1((0.5 - wx.a) * 3.0);
    float convection = smoothstep(0.12, 0.86,
        max(storm, precip * 0.76 + low_pressure * 0.20));

    float base_alt = mix(1450.0, 720.0, convection) - low_pressure * 90.0;
    float fair_top = mix(3500.0, 6200.0, smoothstep(0.16, 0.82, coverage));
    float nominal_top = mix(fair_top, CLOUD_TOP, pow(convection, 0.68));

    vec3 warped_surface = surface_p + cloud_domain_warp(surface_p, wind, convection);
    float top_noise = cloud_fbm(warped_surface,
        CLOUD_SHAPE_SCALE * 0.13, wind * 0.22);
    float top_scale = mix(0.82, 1.12, top_noise);
    float top_alt = min(CLOUD_TOP,
        base_alt + (nominal_top - base_alt) * top_scale);
    if (altitude <= base_alt || altitude >= top_alt) return 0.0;

    float h = saturate1((altitude - base_alt) / max(top_alt - base_alt, 1.0));
    float bottom = smoothstep(0.0, mix(0.085, 0.035, convection), h);
    float top_fade = 1.0 - smoothstep(mix(0.72, 0.86, convection), 1.0, h);
    float vertical = bottom * top_fade;

    float coverage_target = smoothstep(0.08, 0.94, coverage);
    float synoptic_noise = cloud_fbm(warped_surface,
        CLOUD_SHAPE_SCALE * 0.075, wind * 0.16);
    float synoptic_threshold = mix(0.82, 0.46, coverage_target)
        - convection * 0.075;
    float synoptic = smoothstep(synoptic_threshold,
        min(synoptic_threshold + 0.095, 0.995), synoptic_noise);

    float anvil_zone = pow(convection, 1.7)
        * smoothstep(0.67, 0.84, h)
        * (1.0 - smoothstep(0.972, 1.0, h));
    float footprint_noise = cloud_fbm(warped_surface,
        CLOUD_SHAPE_SCALE * 0.31, wind * 0.34);
    float cell_threshold = mix(0.72, 0.45, coverage_target)
        - convection * 0.055 - anvil_zone * 0.075;
    float cells = smoothstep(cell_threshold,
        min(cell_threshold + 0.105, 0.995), footprint_noise);
    float footprint = synoptic * cells;

    vec3 shear = cloud_shear_offset(wind, h, convection);
    float vertical_noise_scale = mix(0.50, 0.22, convection);
    vec3 volume_p = warped_surface + shear
        + radial * (altitude * vertical_noise_scale);
    float meso = cloud_fbm(volume_p, CLOUD_SHAPE_SCALE, wind * 0.74);
    float meso_body = smoothstep(0.33, 0.72, meso);
    float body = footprint * vertical * mix(0.42, 1.20, meso_body);

    float tower_profile = convection
        * smoothstep(0.06, 0.25, h)
        * (1.0 - smoothstep(0.86, 0.985, h));
    float tower_noise = cloud_fbm(
        warped_surface + shear * 1.25 + radial * altitude * 0.16,
        CLOUD_SHAPE_SCALE * 0.66, wind * 0.82);
    float tower = synoptic * tower_profile
        * smoothstep(0.34, 0.68, tower_noise);
    body = max(body, tower * (0.72 + 0.50 * convection));

    float anvil_noise = cloud_fbm(
        warped_surface + shear * 1.60 + radial * altitude * 0.095,
        CLOUD_SHAPE_SCALE * 0.22, wind * 0.55);
    float anvil_spread = smoothstep(
        mix(0.72, 0.52, coverage_target) - convection * 0.08,
        mix(0.82, 0.62, coverage_target) - convection * 0.08,
        anvil_noise);
    float anvil = synoptic * anvil_zone * anvil_spread
        * mix(0.60, 1.08, smoothstep(0.30, 0.72, footprint_noise));
    body = max(body, anvil);

    float upper_weight = smoothstep(0.50, 0.94, h) * vertical;
    float upper_cells = cloud_fbm(volume_p + vec3(1730.0, -410.0, 920.0),
        CLOUD_SHAPE_SCALE * 1.75, wind * 1.08);
    body *= mix(0.84, 1.17, upper_cells * upper_weight);

    float density_gain = mix(0.82, 1.32, convection);
    return smoothstep(0.055, 0.66, max(body, 0.0)) * density_gain;
}

float sun_transmittance(vec3 p, float radius, vec3 sun_dir, vec3 wind) {
    float optical_depth = 0.0;
    float previous_distance = 0.0;
    for (int i = 0; i < LIGHT_STEPS; ++i) {
        float f = float(i + 1) / float(LIGHT_STEPS);
        float distance_m = LIGHT_DISTANCE * (exp2(f * 3.2) - 1.0)
            / (exp2(3.2) - 1.0);
        float dl = distance_m - previous_distance;
        float mid = previous_distance + dl * 0.5;
        optical_depth += canonical_coarse_density(
            p + sun_dir * mid, radius, wind) * dl;
        previous_distance = distance_m;
        if (optical_depth * CLOUD_EXTINCTION > 10.0) break;
    }
    return exp(-optical_depth * CLOUD_EXTINCTION);
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
        float density = canonical_coarse_density(p, radius, wind);
        float direct_t = density > 0.002
            ? sun_transmittance(p, radius, sun_dir, wind)
            : 1.0;

        // Multi-order approximation: deep cloud remains softly illuminated while
        // direct sunlight still drops sharply enough for upper cloud structures to
        // cast visible shadows through lower parts of the same volume.
        float optical = -log(max(direct_t, 1.0e-4));
        float order2 = exp(-optical * 0.48);
        float order3 = exp(-optical * 0.20);
        float trapped = (1.0 - direct_t) * density;
        float multiple = density * (0.014 + 0.048 * order2 + 0.022 * order3)
            * (0.58 + trapped * 0.42);
        float skylight = mix(0.076, 0.020,
            smoothstep(0.0, 1.10, density)) + trapped * 0.020;
        imageStore(light_volume, ivec3(xy, z),
            vec4(direct_t, multiple, skylight, density));
    }
}
