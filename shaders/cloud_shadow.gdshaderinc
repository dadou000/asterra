/// EVE/Kerbin-derived cloud optical-depth shadowing for Asterra.
///
/// WeatherSystem remains authoritative for horizontal cloud/storm placement.
/// The density construction mirrors cloud_depth_composite.glsl closely enough
/// that visible cloud masses and terrain/ocean shadows stay registered. Helion's
/// finite-disc integration is retained for physical penumbra width.

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
uniform int u_cloud_shadow_steps = 6;
uniform vec3 u_cloud_weather_center = vec3(0.0, 1.0, 0.0);
uniform vec3 u_cloud_weather_east = vec3(1.0, 0.0, 0.0);
uniform vec3 u_cloud_weather_north = vec3(0.0, 0.0, 1.0);
uniform float u_cloud_weather_local_span = 422400.0;

/// Physical Helion geometry in metres. Apparent angular radius is derived from
/// radius/distance and therefore follows the authored orbital geometry.
uniform float u_cloud_shadow_helion_radius_m = 696340000.0;
uniform float u_cloud_shadow_helion_distance_m = 149597870700.0;
uniform int u_cloud_shadow_disc_samples = 5;

const int CLOUD_SHADOW_MAX_STEPS = 10;
const int CLOUD_SHADOW_MAX_DISC_SAMPLES = 7;
const float CLOUD_SHADOW_GOLDEN_ANGLE = 2.399963229728653;
const float CLOUD_SHADOW_WORLEY_PERSISTENCE = 0.57;
const float CLOUD_SHADOW_UV_WARP_METRES = 200.0;

vec2 cloud_shadow_sphere_hit(vec3 origin, vec3 dir, float radius) {
    float b = dot(dir, origin);
    float c = dot(origin, origin) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0) return vec2(1e9, -1e9);
    float s = sqrt(disc);
    return vec2(-b - s, -b + s);
}

float cloud_shadow_remap01(float v, float a, float b) {
    return clamp((v - a) / max(b - a, 1e-5), 0.0, 1.0);
}

vec2 cloud_shadow_global_uv(vec3 d) {
    d = normalize(d);
    float lon = atan(d.z, d.x);
    if (lon < 0.0) lon += TAU;
    float lat = asin(clamp(d.y, -1.0, 1.0));
    return vec2(lon / TAU, (PI * 0.5 - lat) / PI);
}

vec4 cloud_shadow_weather(vec3 surface_p, float planet_radius) {
    vec3 d = normalize(surface_p);
    vec4 global_state = textureLod(u_cloud_weather_global,
        cloud_shadow_global_uv(d), 0.0);
    vec3 center = normalize(u_cloud_weather_center);
    vec3 delta = d * planet_radius - center * planet_radius;
    vec2 local_uv = vec2(
        dot(delta, normalize(u_cloud_weather_east)),
        dot(delta, normalize(u_cloud_weather_north)))
        / max(u_cloud_weather_local_span, 1000.0) + vec2(0.5);
    float edge = max(abs(local_uv.x - 0.5), abs(local_uv.y - 0.5));
    float blend = 1.0 - smoothstep(0.42, 0.50, edge);
    vec4 local_state = textureLod(u_cloud_weather_local,
        clamp(local_uv, vec2(0.0), vec2(1.0)), 0.0);
    return mix(global_state, local_state, blend);
}

float cloud_shadow_low_pressure(vec4 wx) {
    return clamp((0.5 - wx.a) * 3.0, 0.0, 1.0);
}

float cloud_shadow_worley_puff(vec3 uv) {
    return 1.0 - textureLod(u_cloud_shadow_shape_noise, uv, 0.0).r;
}

float cloud_shadow_worley_fbm(vec3 p, float scale) {
    vec3 uv = (p + u_cloud_shadow_wind_offset) * scale;
    float n0 = cloud_shadow_worley_puff(uv);
    float n1 = cloud_shadow_worley_puff(
        uv * 2.03 + vec3(0.19, 0.61, 0.43));
    return (n0 + CLOUD_SHADOW_WORLEY_PERSISTENCE * n1)
        / (1.0 + CLOUD_SHADOW_WORLEY_PERSISTENCE);
}

vec3 cloud_shadow_domain_warp(vec3 surface_p) {
    vec3 uv = (surface_p + u_cloud_shadow_wind_offset * 0.16)
        * (u_cloud_shadow_shape_scale * 0.12);
    vec3 q = vec3(
        textureLod(u_cloud_shadow_shape_noise,
            uv + vec3(0.13, 0.47, 0.81), 0.0).r,
        textureLod(u_cloud_shadow_shape_noise,
            uv + vec3(0.71, 0.23, 0.37), 0.0).r,
        textureLod(u_cloud_shadow_shape_noise,
            uv + vec3(0.41, 0.89, 0.17), 0.0).r);
    return (q * 2.0 - 1.0) * CLOUD_SHADOW_UV_WARP_METRES;
}

float cloud_shadow_coverage_remap(float coverage, float convective) {
    float stratus_curve = smoothstep(0.035, 0.72, coverage);
    float cumulus_curve = smoothstep(0.020, 0.58, coverage);
    return mix(stratus_curve, cumulus_curve, clamp(convective, 0.0, 1.0));
}

float cloud_shadow_edge_width(float hardness) {
    return mix(0.23, 0.045, clamp(hardness, 0.0, 1.0));
}

float cloud_shadow_shape(float macro, float coverage, float hardness) {
    float threshold = 1.0 - clamp(coverage, 0.0, 1.0) * 0.76;
    float width = cloud_shadow_edge_width(hardness);
    return smoothstep(threshold, min(threshold + width, 0.999), macro);
}

float cloud_shadow_band(float altitude, float base_alt, float top_alt,
        float bottom_soft, float top_soft) {
    if (altitude <= base_alt || altitude >= top_alt) return 0.0;
    float bottom = smoothstep(base_alt, base_alt + bottom_soft, altitude);
    float top = 1.0 - smoothstep(top_alt - top_soft, top_alt, altitude);
    return bottom * top;
}

float cloud_shadow_fair_top(float t) {
    if (t < 0.20) return mix(3450.0, 4450.0, t / 0.20);
    if (t < 0.40) return mix(4450.0, 5450.0, (t - 0.20) / 0.20);
    if (t < 0.60) return mix(5450.0, 6450.0, (t - 0.40) / 0.20);
    if (t < 0.80) return mix(6450.0, 7450.0, (t - 0.60) / 0.20);
    return mix(7450.0, 9300.0, (t - 0.80) / 0.20);
}

float cloud_shadow_density_at(vec3 p, float planet_radius) {
    float altitude = length(p) - planet_radius;
    if (altitude <= u_cloud_shadow_base || altitude >= u_cloud_shadow_top) {
        return 0.0;
    }

    vec4 wx = cloud_shadow_weather(normalize(p) * planet_radius, planet_radius);
    float coverage = clamp(wx.r, 0.0, 1.0);
    float storm = clamp(wx.g, 0.0, 1.0);
    float precip = clamp(wx.b, 0.0, 1.0);
    float lowp = cloud_shadow_low_pressure(wx);

    float type = clamp(storm * 1.18 + precip * 0.10
        + max(coverage - 0.62, 0.0) * 0.18, 0.0, 1.0);
    float fair_coverage = cloud_shadow_coverage_remap(coverage, type);
    float fair_hardness = mix(0.90, 0.97, smoothstep(0.08, 0.35, type));
    float fair_top = cloud_shadow_fair_top(type);
    float fair_profile = cloud_shadow_band(altitude,
        mix(1100.0, 1000.0, type), fair_top, 180.0,
        mix(600.0, 1050.0, type));

    vec3 surface_p = normalize(p) * planet_radius;
    vec3 warped_p = p + cloud_shadow_domain_warp(surface_p);
    float fair_macro = cloud_shadow_worley_fbm(
        warped_p, u_cloud_shadow_shape_scale);
    float fair_body = cloud_shadow_shape(
        fair_macro, fair_coverage, fair_hardness) * fair_profile;
    float fair_optical = smoothstep(0.006, 0.24, fair_body)
        * mix(0.50, 1.05, smoothstep(0.08, 0.34, type));

    float deep = smoothstep(0.42, 0.82,
        max(storm, precip * 0.70 + lowp * 0.22));
    float storm_coverage = clamp(
        coverage * 0.72 + storm * 0.46 + lowp * 0.12, 0.0, 1.0);

    float core_macro = cloud_shadow_worley_fbm(
        warped_p, u_cloud_shadow_shape_scale * 0.77);
    float core = cloud_shadow_shape(core_macro, storm_coverage, 0.96)
        * cloud_shadow_band(altitude, 1150.0, u_cloud_shadow_top,
            220.0, 1050.0);

    float edge_macro = cloud_shadow_worley_fbm(
        warped_p, u_cloud_shadow_shape_scale * 0.58);
    float edge_mass = cloud_shadow_shape(
        edge_macro, clamp(storm_coverage * 0.88, 0.0, 1.0), 0.96)
        * cloud_shadow_band(altitude, 1000.0, 13200.0,
            260.0, 1500.0);

    float trail_macro = cloud_shadow_worley_fbm(
        warped_p, u_cloud_shadow_shape_scale * 0.045);
    float trail = cloud_shadow_shape(
        trail_macro, clamp(storm_coverage * 0.72, 0.0, 1.0), 0.50);
    trail *= exp(-pow((altitude - 11250.0) / 2100.0, 2.0))
        * smoothstep(0.48, 0.78, deep);

    float storm_body = max(max(core, edge_mass * 0.72), trail * 0.48);
    float storm_optical = smoothstep(0.005, 0.22, storm_body)
        * deep * (0.85 + 0.45 * storm);

    return (fair_optical + storm_optical) * u_cloud_shadow_density;
}

vec2 cloud_shadow_layer_segment(vec3 origin, vec3 dir, float planet_radius) {
    float inner_radius = planet_radius + u_cloud_shadow_base;
    float outer_radius = planet_radius + u_cloud_shadow_top;
    vec2 outer_hit = cloud_shadow_sphere_hit(origin, dir, outer_radius);
    if (outer_hit.y <= 0.0) return vec2(1e9, -1e9);
    float source_radius = length(origin);
    float ray_start = max(outer_hit.x, 0.0);
    float ray_end = outer_hit.y;
    vec2 inner_hit = cloud_shadow_sphere_hit(origin, dir, inner_radius);
    if (source_radius < inner_radius) {
        ray_start = max(ray_start, inner_hit.y);
    } else if (source_radius < outer_radius) {
        ray_start = 0.0;
        if (inner_hit.x > 0.0 && inner_hit.x < ray_end) ray_end = inner_hit.x;
    } else if (inner_hit.x > ray_start && inner_hit.x < ray_end) {
        ray_end = inner_hit.x;
    }
    if (ray_end <= ray_start) return vec2(1e9, -1e9);
    return vec2(ray_start, ray_end);
}

float cloud_shadow_ray_transmittance(vec3 receiver, vec3 ray_dir,
        float planet_radius) {
    vec2 segment = cloud_shadow_layer_segment(receiver, ray_dir, planet_radius);
    if (segment.x > segment.y) return 1.0;
    int steps = clamp(u_cloud_shadow_steps, 2, CLOUD_SHADOW_MAX_STEPS);
    float path_length = segment.y - segment.x;
    float step_len = path_length / float(steps);
    float optical_depth = 0.0;
    for (int i = 0; i < CLOUD_SHADOW_MAX_STEPS; i++) {
        if (i >= steps) break;
        float t = segment.x + (float(i) + 0.5) * step_len;
        vec3 p = receiver + ray_dir * t;
        optical_depth += cloud_shadow_density_at(p, planet_radius) * step_len;
        if (optical_depth * u_cloud_shadow_extinction > 9.0) return 0.00012;
    }
    return exp(-optical_depth * u_cloud_shadow_extinction);
}

float cloud_shadow_transmittance(vec3 receiver, vec3 sun_dir,
        float planet_radius) {
    if (u_cloud_shadow_enabled < 0.5) return 1.0;
    vec3 s = normalize(sun_dir);
    int disc_samples = clamp(u_cloud_shadow_disc_samples,
        1, CLOUD_SHADOW_MAX_DISC_SAMPLES);
    if (disc_samples <= 1) {
        return cloud_shadow_ray_transmittance(receiver, s, planet_radius);
    }

    float radius_distance_ratio = clamp(
        u_cloud_shadow_helion_radius_m
            / max(u_cloud_shadow_helion_distance_m, 1.0),
        0.0, 0.999999);
    float angular_radius = asin(radius_distance_ratio);
    float disc_slope = tan(angular_radius);
    if (disc_slope < 1e-7) {
        return cloud_shadow_ray_transmittance(receiver, s, planet_radius);
    }

    vec3 reference_axis = abs(s.z) < 0.999
        ? vec3(0.0, 0.0, 1.0)
        : vec3(0.0, 1.0, 0.0);
    vec3 tangent = normalize(cross(reference_axis, s));
    vec3 bitangent = normalize(cross(s, tangent));
    float transmittance_sum = 0.0;
    for (int j = 0; j < CLOUD_SHADOW_MAX_DISC_SAMPLES; j++) {
        if (j >= disc_samples) break;
        float sample_index = float(j) + 0.5;
        float radial = sqrt(sample_index / float(disc_samples));
        float phi = sample_index * CLOUD_SHADOW_GOLDEN_ANGLE;
        vec2 disc_point = vec2(cos(phi), sin(phi)) * radial;
        vec3 ray_dir = normalize(s
            + (tangent * disc_point.x + bitangent * disc_point.y) * disc_slope);
        transmittance_sum += cloud_shadow_ray_transmittance(
            receiver, ray_dir, planet_radius);
    }
    return transmittance_sum / float(disc_samples);
}
