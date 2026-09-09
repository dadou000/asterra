/// Weather-driven volumetric cloud shadows plus accumulated wet-surface response.
/// Macro morphology mirrors cloud_depth_composite.glsl and cloud_light_volume.glsl.
/// Surface shadows are an optical-depth projection of the same continuous 3D body.
uniform sampler3D u_cloud_shadow_shape_noise : repeat_enable, filter_linear;
uniform sampler2D u_cloud_weather_global : filter_linear;
uniform sampler2D u_cloud_weather_local : filter_linear;
uniform float u_cloud_shadow_enabled = 0.0;
uniform float u_cloud_shadow_base = 800.0;
uniform float u_cloud_shadow_top = 14500.0;
uniform float u_cloud_shadow_coverage = 0.28;
uniform float u_cloud_shadow_weather_variation = 0.30;
uniform float u_cloud_shadow_density = 1.0;
uniform float u_cloud_shadow_shape_scale = 0.000052;
uniform float u_cloud_shadow_weather_scale = 0.0000085;
uniform float u_cloud_shadow_extinction = 0.0010;
uniform vec3 u_cloud_shadow_wind_offset = vec3(0.0);
uniform vec3 u_cloud_shadow_sun_dir = vec3(1.0, 0.0, 0.0);
uniform int u_cloud_shadow_steps = 8;
uniform vec3 u_cloud_weather_center = vec3(0.0, 1.0, 0.0);
uniform vec3 u_cloud_weather_east = vec3(1.0, 0.0, 0.0);
uniform vec3 u_cloud_weather_north = vec3(0.0, 0.0, 1.0);
uniform float u_cloud_weather_local_span = 422400.0;
uniform float u_weather_surface_wetness : hint_range(0.0, 1.0) = 0.0;
uniform float u_cloud_shadow_helion_radius_m = 696340000.0;
uniform float u_cloud_shadow_helion_distance_m = 149597870700.0;
uniform int u_cloud_shadow_disc_samples = 5;

const int CLOUD_SHADOW_MAX_STEPS = 12;
const int CLOUD_SHADOW_MAX_DISC_SAMPLES = 7;
const float CLOUD_SHADOW_GOLDEN_ANGLE = 2.399963229728653;
const float CLOUD_SHADOW_WORLEY_PERSISTENCE = 0.57;

float cloud_shadow_saturate(float x) { return clamp(x, 0.0, 1.0); }

vec2 cloud_shadow_sphere_hit(vec3 o, vec3 d, float r) {
    float b = dot(d, o);
    float c = dot(o, o) - r * r;
    float q = b * b - c;
    if (q < 0.0) return vec2(1e9, -1e9);
    float s = sqrt(q);
    return vec2(-b - s, -b + s);
}

vec2 cloud_shadow_global_uv(vec3 d) {
    d = normalize(d);
    float lon = atan(d.z, d.x);
    if (lon < 0.0) lon += TAU;
    float lat = asin(clamp(d.y, -1.0, 1.0));
    return vec2(lon / TAU, (PI * 0.5 - lat) / PI);
}

void cloud_shadow_local_basis(out vec3 center, out vec3 east, out vec3 north) {
    center = normalize(u_cloud_weather_center);
    vec3 pole = abs(center.y) > 0.92 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    east = normalize(cross(pole, center));
    north = normalize(cross(center, east));
}

bool cloud_shadow_uv_inside_01(vec2 uv) {
    return all(greaterThanEqual(uv, vec2(0.0)))
        && all(lessThanEqual(uv, vec2(1.0)));
}

vec4 cloud_shadow_weather(vec3 surface_p, float radius) {
    vec3 d = normalize(surface_p);
    vec4 g = textureLod(u_cloud_weather_global, cloud_shadow_global_uv(d), 0.0);
    vec3 center, east, north;
    cloud_shadow_local_basis(center, east, north);
    vec3 delta = d * radius - center * radius;
    vec2 uv = vec2(dot(delta, east), dot(delta, north))
        / max(u_cloud_weather_local_span, 1000.0) + vec2(0.5);
    if (!cloud_shadow_uv_inside_01(uv)) return g;
    float edge = max(abs(uv.x - 0.5), abs(uv.y - 0.5));
    float blend = 1.0 - smoothstep(0.40, 0.48, edge);
    vec4 l = textureLod(u_cloud_weather_local, uv, 0.0);
    return mix(g, l, blend);
}

float cloud_shadow_cell(vec3 uv) {
    return 1.0 - textureLod(u_cloud_shadow_shape_noise, uv, 0.0).r;
}

float cloud_shadow_fbm(vec3 p, float scale, vec3 advect) {
    vec3 uv = (p + advect) * scale;
    float n0 = cloud_shadow_cell(uv);
    float n1 = cloud_shadow_cell(uv * 2.03 + vec3(0.19, 0.61, 0.43));
    float n2 = cloud_shadow_cell(uv * 4.11 + vec3(0.73, 0.31, 0.11));
    float p1 = CLOUD_SHADOW_WORLEY_PERSISTENCE;
    float p2 = p1 * p1;
    return (n0 + p1 * n1 + p2 * n2) / (1.0 + p1 + p2);
}

vec3 cloud_shadow_domain_warp(vec3 surface_p, float convection) {
    vec3 uv = (surface_p + u_cloud_shadow_wind_offset * 0.18)
        * (u_cloud_shadow_shape_scale * 0.16);
    vec3 q = vec3(
        cloud_shadow_cell(uv + vec3(0.13, 0.47, 0.81)),
        cloud_shadow_cell(uv + vec3(0.71, 0.23, 0.37)),
        cloud_shadow_cell(uv + vec3(0.41, 0.89, 0.17)));
    float metres = mix(720.0, 1650.0, convection);
    return (q * 2.0 - 1.0) * metres;
}

vec3 cloud_shadow_shear_offset(float h, float convection) {
    float upper = h * h;
    vec3 wind = u_cloud_shadow_wind_offset;
    vec3 cross_wind = vec3(-wind.z, wind.y * 0.12, wind.x);
    return wind * upper * mix(0.08, 0.28, convection)
        + cross_wind * upper * mix(0.025, 0.12, convection);
}

float cloud_shadow_density_at(vec3 p, float radius) {
    float altitude = length(p) - radius;
    if (altitude <= 0.0 || altitude >= u_cloud_shadow_top) return 0.0;

    vec3 radial = normalize(p);
    vec3 surface_p = radial * radius;
    vec4 wx = cloud_shadow_weather(surface_p, radius);
    float coverage = cloud_shadow_saturate(wx.r);
    float storm = cloud_shadow_saturate(wx.g);
    float precip = cloud_shadow_saturate(wx.b);
    float low_pressure = cloud_shadow_saturate((0.5 - wx.a) * 3.0);
    float convection = smoothstep(0.12, 0.86,
        max(storm, precip * 0.76 + low_pressure * 0.20));

    float base_alt = mix(1450.0, 720.0, convection) - low_pressure * 90.0;
    float fair_top = mix(3500.0, 6200.0, smoothstep(0.16, 0.82, coverage));
    float nominal_top = mix(fair_top, u_cloud_shadow_top, pow(convection, 0.68));

    vec3 warped_surface = surface_p + cloud_shadow_domain_warp(surface_p, convection);
    float top_noise = cloud_shadow_fbm(warped_surface,
        u_cloud_shadow_shape_scale * 0.13, u_cloud_shadow_wind_offset * 0.22);
    float top_scale = mix(0.82, 1.12, top_noise);
    float top_alt = min(u_cloud_shadow_top,
        base_alt + (nominal_top - base_alt) * top_scale);
    if (altitude <= base_alt || altitude >= top_alt) return 0.0;

    float h = cloud_shadow_saturate((altitude - base_alt)
        / max(top_alt - base_alt, 1.0));
    float bottom = smoothstep(0.0, mix(0.085, 0.035, convection), h);
    float top_fade = 1.0 - smoothstep(mix(0.72, 0.86, convection), 1.0, h);
    float vertical = bottom * top_fade;

    float coverage_target = smoothstep(0.08, 0.94, coverage);
    float synoptic_noise = cloud_shadow_fbm(warped_surface,
        u_cloud_shadow_shape_scale * 0.075, u_cloud_shadow_wind_offset * 0.16);
    float synoptic_threshold = mix(0.82, 0.46, coverage_target)
        - convection * 0.075;
    float synoptic = smoothstep(synoptic_threshold,
        min(synoptic_threshold + 0.095, 0.995), synoptic_noise);

    float anvil_zone = pow(convection, 1.7)
        * smoothstep(0.67, 0.84, h)
        * (1.0 - smoothstep(0.972, 1.0, h));
    float footprint_noise = cloud_shadow_fbm(warped_surface,
        u_cloud_shadow_shape_scale * 0.31, u_cloud_shadow_wind_offset * 0.34);
    float cell_threshold = mix(0.72, 0.45, coverage_target)
        - convection * 0.055 - anvil_zone * 0.075;
    float cells = smoothstep(cell_threshold,
        min(cell_threshold + 0.105, 0.995), footprint_noise);
    float footprint = synoptic * cells;

    vec3 shear = cloud_shadow_shear_offset(h, convection);
    float vertical_noise_scale = mix(0.50, 0.22, convection);
    vec3 volume_p = warped_surface + shear
        + radial * (altitude * vertical_noise_scale);
    float meso = cloud_shadow_fbm(volume_p, u_cloud_shadow_shape_scale,
        u_cloud_shadow_wind_offset * 0.74);
    float meso_body = smoothstep(0.33, 0.72, meso);
    float body = footprint * vertical * mix(0.42, 1.20, meso_body);

    float tower_profile = convection
        * smoothstep(0.06, 0.25, h)
        * (1.0 - smoothstep(0.86, 0.985, h));
    float tower_noise = cloud_shadow_fbm(
        warped_surface + shear * 1.25 + radial * altitude * 0.16,
        u_cloud_shadow_shape_scale * 0.66,
        u_cloud_shadow_wind_offset * 0.82);
    float tower = synoptic * tower_profile
        * smoothstep(0.34, 0.68, tower_noise);
    body = max(body, tower * (0.72 + 0.50 * convection));

    float anvil_noise = cloud_shadow_fbm(
        warped_surface + shear * 1.60 + radial * altitude * 0.095,
        u_cloud_shadow_shape_scale * 0.22,
        u_cloud_shadow_wind_offset * 0.55);
    float anvil_spread = smoothstep(
        mix(0.72, 0.52, coverage_target) - convection * 0.08,
        mix(0.82, 0.62, coverage_target) - convection * 0.08,
        anvil_noise);
    float anvil = synoptic * anvil_zone * anvil_spread
        * mix(0.60, 1.08, smoothstep(0.30, 0.72, footprint_noise));
    body = max(body, anvil);

    float upper_weight = smoothstep(0.50, 0.94, h) * vertical;
    float upper_cells = cloud_shadow_fbm(
        volume_p + vec3(1730.0, -410.0, 920.0),
        u_cloud_shadow_shape_scale * 1.75,
        u_cloud_shadow_wind_offset * 1.08);
    body *= mix(0.84, 1.17, upper_cells * upper_weight);

    float density_gain = mix(0.82, 1.32, convection);
    return smoothstep(0.055, 0.66, max(body, 0.0))
        * density_gain * u_cloud_shadow_density;
}

vec2 cloud_shadow_layer_segment(vec3 o, vec3 d, float radius) {
    vec2 outer = cloud_shadow_sphere_hit(o, d, radius + u_cloud_shadow_top);
    if (outer.y <= 0.0) return vec2(1e9, -1e9);
    float start = max(outer.x, 0.0);
    float end = outer.y;
    vec2 ground = cloud_shadow_sphere_hit(o, d, radius);
    if (ground.x > 0.0) end = min(end, ground.x);
    if (end <= start) return vec2(1e9, -1e9);
    return vec2(start, end);
}

float cloud_shadow_ray_transmittance(vec3 receiver, vec3 ray_dir, float radius) {
    vec2 seg = cloud_shadow_layer_segment(receiver, ray_dir, radius);
    if (seg.x > seg.y) return 1.0;

    int steps = clamp(u_cloud_shadow_steps, 2, CLOUD_SHADOW_MAX_STEPS);
    float dl = (seg.y - seg.x) / float(steps);
    float optical_depth = 0.0;
    for (int i = 0; i < CLOUD_SHADOW_MAX_STEPS; ++i) {
        if (i >= steps) break;
        vec3 p = receiver + ray_dir * (seg.x + (float(i) + 0.5) * dl);
        optical_depth += cloud_shadow_density_at(p, radius) * dl;
        if (optical_depth * u_cloud_shadow_extinction > 9.0)
            return 0.00012;
    }
    return exp(-optical_depth * u_cloud_shadow_extinction);
}

float cloud_shadow_wet_response(float transmittance) {
    return transmittance * mix(1.0, 0.86,
        clamp(u_weather_surface_wetness, 0.0, 1.0));
}

float cloud_shadow_transmittance(vec3 receiver, vec3 sun_dir, float radius) {
    if (u_cloud_shadow_enabled < 0.5)
        return cloud_shadow_wet_response(1.0);

    vec3 s = normalize(sun_dir);
    int count = clamp(u_cloud_shadow_disc_samples, 1,
        CLOUD_SHADOW_MAX_DISC_SAMPLES);
    if (count <= 1) {
        return cloud_shadow_wet_response(
            cloud_shadow_ray_transmittance(receiver, s, radius));
    }

    float ratio = clamp(u_cloud_shadow_helion_radius_m
        / max(u_cloud_shadow_helion_distance_m, 1.0), 0.0, 0.999999);
    float slope = tan(asin(ratio));
    if (slope < 1e-7) {
        return cloud_shadow_wet_response(
            cloud_shadow_ray_transmittance(receiver, s, radius));
    }

    vec3 axis = abs(s.z) < 0.999
        ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 tangent = normalize(cross(axis, s));
    vec3 bitangent = normalize(cross(s, tangent));
    float sum = 0.0;
    for (int j = 0; j < CLOUD_SHADOW_MAX_DISC_SAMPLES; ++j) {
        if (j >= count) break;
        float k = float(j) + 0.5;
        float radial = sqrt(k / float(count));
        float phi = k * CLOUD_SHADOW_GOLDEN_ANGLE;
        vec2 q = vec2(cos(phi), sin(phi)) * radial;
        vec3 rd = normalize(s + (tangent * q.x + bitangent * q.y) * slope);
        sum += cloud_shadow_ray_transmittance(receiver, rd, radius);
    }
    return cloud_shadow_wet_response(sum / float(count));
}
