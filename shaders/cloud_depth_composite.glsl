#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform image2D color_image;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 0, binding = 2) uniform sampler3D shape_noise;
layout(set = 0, binding = 3) uniform sampler3D detail_noise;
layout(set = 0, binding = 4) uniform sampler2D global_weather;
layout(set = 0, binding = 5) uniform sampler2D local_weather;

// Exactly 128 bytes. Keep this layout in sync with CloudDepthCompositorEffect.
layout(push_constant, std430) uniform Params {
    vec4 camera_planet_radius;
    vec4 sun_dir_intensity;
    // xyz = wind metres, w = integer primary steps + fractional Helion angular radius.
    vec4 wind_steps;
    vec4 weather_center_span;
    mat4 inv_world_projection;
} params;

const float PI = 3.14159265358979323846;
const float ATMOSPHERE_TOP = 60000.0;

// EVE/Kerbin-derived cloud-domain tuning, adapted to Asterra.
// Asterra is 1000 km radius versus Kerbin's 600 km. Horizontal cell dimensions
// and march distances are scaled by 5/3 so the same structures subtend similar
// angles. Vertical cloud heights remain terrestrial and are driven by Asterra's
// live weather simulation rather than scaled with planet radius.
const float CLOUD_SHELL_BASE = 0.0;
const float CLOUD_TOP = 14500.0;
const float CLOUD_FAIR_BASE = 1100.0;
const float CLOUD_STORM_BASE = 900.0;
const float CLOUD_DENSITY = 1.0;
const float CLOUD_SHAPE_SCALE = 0.0000520;     // ~3.8 km base Worley cell
const float CLOUD_DETAIL_SCALE = 0.00042;
const float CLOUD_DETAIL_EROSION = 0.65;       // Kerbin erosionDepth
const float CLOUD_EXTINCTION = 0.0010;
const float CLOUD_WORLEY_PERSISTENCE = 0.57;   // Kerbin worley.persistence
const float CLOUD_BASE_STEP = 125.0;           // Kerbin 75 m * 5/3
const float CLOUD_ADAPTIVE_FACTOR = 0.006;
const float CLOUD_MAX_STEP = 2500.0;            // Kerbin 1500 m * 5/3
const float CLOUD_LIGHT_DISTANCE = 2000.0;      // Kerbin 1200 m * 5/3
const float CLOUD_UV_WARP_METRES = 200.0;       // ~Kerbin _UVNoiseStrength angular scale
const int CLOUD_MAX_PRIMARY_STEPS = 28;
const int CLOUD_MAX_LIGHT_STEPS = 4;
const int AIR_STEPS = 4;

vec2 sphere_intersect(vec3 origin, vec3 dir, float radius) {
    float b = dot(dir, origin);
    float c = dot(origin, origin) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.0) return vec2(1e30, -1e30);
    float s = sqrt(disc);
    return vec2(-b - s, -b + s);
}

float remap01(float v, float a, float b) {
    return clamp((v - a) / max(b - a, 1e-5), 0.0, 1.0);
}

float hash13(vec3 p) {
    return fract(sin(dot(p, vec3(91.17, 37.53, 141.73))) * 43758.5453);
}

vec2 global_weather_uv(vec3 d) {
    d = normalize(d);
    float lon = atan(d.z, d.x);
    if (lon < 0.0) lon += 2.0 * PI;
    float lat = asin(clamp(d.y, -1.0, 1.0));
    return vec2(lon / (2.0 * PI), (PI * 0.5 - lat) / PI);
}

// WeatherSystem packing:
// R cloud fraction, G organised convection/storm, B precipitation,
// A lower-atmosphere pressure anomaly encoded around 0.5.
vec4 weather_state(vec3 surface_p, float radius) {
    vec3 d = normalize(surface_p);
    vec4 g = textureLod(global_weather, global_weather_uv(d), 0.0);
    vec3 center = normalize(params.weather_center_span.xyz);
    vec3 pole = abs(center.y) > 0.92 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 east = normalize(cross(pole, center));
    vec3 north = normalize(cross(center, east));
    vec3 tangent_delta = d * radius - center * radius;
    float span = max(params.weather_center_span.w, 1000.0);
    vec2 luv = vec2(dot(tangent_delta, east), dot(tangent_delta, north)) / span + vec2(0.5);
    float edge = max(abs(luv.x - 0.5), abs(luv.y - 0.5));
    float local_weight = 1.0 - smoothstep(0.42, 0.50, edge);
    vec4 l = textureLod(local_weather, clamp(luv, vec2(0.0), vec2(1.0)), 0.0);
    return mix(g, l, local_weight);
}

float low_pressure_weight(vec4 wx) {
    return clamp((0.5 - wx.a) * 3.0, 0.0, 1.0);
}

// NoiseTexture3D is generated as cellular distance. Invert it to get the rounded
// "puff" masses used by the EVE/Nubis-style Worley construction.
float worley_puff(vec3 uv) {
    return 1.0 - textureLod(shape_noise, uv, 0.0).r;
}

float worley_fbm(vec3 p, float scale, vec3 wind) {
    vec3 uv = (p + wind) * scale;
    float n0 = worley_puff(uv);
    float n1 = worley_puff(uv * 2.03 + vec3(0.19, 0.61, 0.43));
    return (n0 + CLOUD_WORLEY_PERSISTENCE * n1) / (1.0 + CLOUD_WORLEY_PERSISTENCE);
}

// Cheap 3-axis domain warp corresponding to EVE's uvnoise1. It is intentionally
// low frequency and only displaces the macro field by a few hundred metres.
vec3 cloud_domain_warp(vec3 surface_p, vec3 wind) {
    vec3 uv = (surface_p + wind * 0.16) * (CLOUD_SHAPE_SCALE * 0.12);
    vec3 q = vec3(
        textureLod(shape_noise, uv + vec3(0.13, 0.47, 0.81), 0.0).r,
        textureLod(shape_noise, uv + vec3(0.71, 0.23, 0.37), 0.0).r,
        textureLod(shape_noise, uv + vec3(0.41, 0.89, 0.17), 0.0).r);
    return (q * 2.0 - 1.0) * CLOUD_UV_WARP_METRES;
}

float coverage_remap(float coverage, float convective) {
    // WeatherSystem already provides physical cloud fraction, unlike EVE's
    // art-authored AlphaMap. Preserve that meaning while applying the sharp
    // onset characteristic of the Kerbin coverage curves.
    float stratus_curve = smoothstep(0.035, 0.72, coverage);
    float cumulus_curve = smoothstep(0.020, 0.58, coverage);
    return mix(stratus_curve, cumulus_curve, clamp(convective, 0.0, 1.0));
}

float edge_width_from_hardness(float hardness) {
    return mix(0.23, 0.045, clamp(hardness, 0.0, 1.0));
}

float shaped_worley(float macro, float coverage, float hardness) {
    float threshold = 1.0 - clamp(coverage, 0.0, 1.0) * 0.76;
    float width = edge_width_from_hardness(hardness);
    return smoothstep(threshold, min(threshold + width, 0.999), macro);
}

float band_profile(float altitude, float base_alt, float top_alt, float bottom_soft, float top_soft) {
    if (altitude <= base_alt || altitude >= top_alt) return 0.0;
    float bottom = smoothstep(base_alt, base_alt + bottom_soft, altitude);
    float top = 1.0 - smoothstep(top_alt - top_soft, top_alt, altitude);
    return bottom * top;
}

float fair_cloud_top(float t) {
    // Kerbin cloudTypes adapted to Asterra's terrestrial-height atmosphere:
    // Stratus 3.45 km, Cumulus 4.45 km, then progressively taller Congestus.
    if (t < 0.20) return mix(3450.0, 4450.0, t / 0.20);
    if (t < 0.40) return mix(4450.0, 5450.0, (t - 0.20) / 0.20);
    if (t < 0.60) return mix(5450.0, 6450.0, (t - 0.40) / 0.20);
    if (t < 0.80) return mix(6450.0, 7450.0, (t - 0.60) / 0.20);
    return mix(7450.0, 9300.0, (t - 0.80) / 0.20);
}

float fair_density(vec3 p, float altitude, vec4 wx, vec3 wind, bool with_detail, float detail_weight) {
    float coverage = clamp(wx.r, 0.0, 1.0);
    float storm = clamp(wx.g, 0.0, 1.0);
    float precip = clamp(wx.b, 0.0, 1.0);
    float type = clamp(storm * 1.18 + precip * 0.10 + max(coverage - 0.62, 0.0) * 0.18, 0.0, 1.0);
    float effective_coverage = coverage_remap(coverage, type);
    float hardness = mix(0.90, 0.97, smoothstep(0.08, 0.35, type));
    float top = fair_cloud_top(type);
    float base_alt = mix(CLOUD_FAIR_BASE, 1000.0, type);
    float profile = band_profile(altitude, base_alt, top, 180.0, mix(600.0, 1050.0, type));
    if (profile <= 0.0 || effective_coverage <= 0.001) return 0.0;

    vec3 surface_p = normalize(p) * (length(p) - altitude);
    vec3 warped_p = p + cloud_domain_warp(surface_p, wind);
    float macro = worley_fbm(warped_p, CLOUD_SHAPE_SCALE, wind);
    float body = shaped_worley(macro, effective_coverage, hardness) * profile;

    if (with_detail && detail_weight > 0.001 && body > 0.006) {
        vec3 detail_p = (p + wind * 1.31) * CLOUD_DETAIL_SCALE;
        float detail = textureLod(detail_noise, detail_p + vec3(0.41, 0.17, 0.83), 0.0).r;
        float edge = 1.0 - smoothstep(0.24, 0.88, body);
        body = max(body - (1.0 - detail) * CLOUD_DETAIL_EROSION
            * detail_weight * (0.18 + 0.82 * edge) * (1.0 - body), 0.0);
    }

    // Preserve EVE's much lower Stratus optical density relative to Cumulus,
    // without importing its engine-specific raw density units directly.
    float optical_scale = mix(0.50, 1.05, smoothstep(0.08, 0.34, type));
    return smoothstep(0.006, 0.24, body) * optical_scale;
}

float storm_density(vec3 p, float altitude, vec4 wx, vec3 wind, bool with_detail, float detail_weight) {
    float coverage = clamp(wx.r, 0.0, 1.0);
    float storm = clamp(wx.g, 0.0, 1.0);
    float precip = clamp(wx.b, 0.0, 1.0);
    float lowp = low_pressure_weight(wx);
    float deep = smoothstep(0.42, 0.82, max(storm, precip * 0.70 + lowp * 0.22));
    if (deep <= 0.001 || altitude <= CLOUD_STORM_BASE || altitude >= CLOUD_TOP) return 0.0;

    float storm_coverage = clamp(coverage * 0.72 + storm * 0.46 + lowp * 0.12, 0.0, 1.0);
    vec3 surface_p = normalize(p) * (length(p) - altitude);
    vec3 warped_p = p + cloud_domain_warp(surface_p, wind);

    // Kerbin Cb "core": 3 km tiling -> ~5 km adapted horizontal cell, hard edge.
    float core_macro = worley_fbm(warped_p, CLOUD_SHAPE_SCALE * 0.77, wind);
    float core = shaped_worley(core_macro, storm_coverage, 0.96);
    float core_profile = band_profile(altitude, 1150.0, CLOUD_TOP, 220.0, 1050.0);
    core *= core_profile;

    // Kerbin Cb "edge": 4 km tiling -> ~6.7 km adapted horizontal cell.
    float edge_macro = worley_fbm(warped_p, CLOUD_SHAPE_SCALE * 0.58, wind);
    float edge_mass = shaped_worley(edge_macro, clamp(storm_coverage * 0.88, 0.0, 1.0), 0.96);
    edge_mass *= band_profile(altitude, 1000.0, 13200.0, 260.0, 1500.0);

    // Kerbin Cb "trail": very large (~50 km) cells and soft edge. This becomes
    // the broad anvil/pressure-system shield around the convective core.
    float trail_macro = worley_fbm(warped_p, CLOUD_SHAPE_SCALE * 0.045, wind * 0.35);
    float trail = shaped_worley(trail_macro, clamp(storm_coverage * 0.72, 0.0, 1.0), 0.50);
    float anvil_profile = exp(-pow((altitude - 11250.0) / 2100.0, 2.0));
    trail *= anvil_profile * smoothstep(0.48, 0.78, deep);

    if (with_detail && detail_weight > 0.001) {
        vec3 detail_p = (p + wind * 1.31) * CLOUD_DETAIL_SCALE;
        float detail = textureLod(detail_noise, detail_p + vec3(0.17, 0.73, 0.49), 0.0).r;
        float body_for_edge = max(core, edge_mass);
        float erosion_edge = 1.0 - smoothstep(0.28, 0.90, body_for_edge);
        float erosion = (1.0 - detail) * CLOUD_DETAIL_EROSION * detail_weight
            * (0.12 + 0.88 * erosion_edge) * (1.0 - body_for_edge);
        core = max(core - erosion, 0.0);
        edge_mass = max(edge_mass - erosion * 0.72, 0.0);
    }

    float developed = max(core, edge_mass * 0.72);
    float body = max(developed, trail * 0.48);
    return smoothstep(0.005, 0.22, body) * deep * (0.85 + 0.45 * storm);
}

float rain_haze_density(vec3 p, float altitude, vec4 wx, vec3 wind) {
    float precip = clamp(wx.b, 0.0, 1.0);
    if (precip < 0.05 || altitude < 0.0 || altitude > 2300.0) return 0.0;
    float profile = smoothstep(0.0, 180.0, altitude)
        * (1.0 - smoothstep(1500.0, 2300.0, altitude));
    float macro = worley_fbm(p, CLOUD_SHAPE_SCALE * 0.13, wind * 0.55);
    return precip * profile * mix(0.035, 0.14, macro);
}

float density_field(vec3 p, float radius, vec3 wind, bool with_detail, float detail_weight) {
    float altitude = length(p) - radius;
    if (altitude < CLOUD_SHELL_BASE || altitude >= CLOUD_TOP) return 0.0;
    vec4 wx = weather_state(normalize(p) * radius, radius);
    float fair = fair_density(p, altitude, wx, wind, with_detail, detail_weight);
    float storm = storm_density(p, altitude, wx, wind, with_detail, detail_weight);
    float haze = rain_haze_density(p, altitude, wx, wind);
    return (fair + storm + haze) * CLOUD_DENSITY;
}

float density_coarse(vec3 p, float radius, vec3 wind) {
    // Lighting needs optical mass, not edge detail. Keep this path deliberately
    // cheap because it is evaluated four times for every lit visible sample.
    float altitude = length(p) - radius;
    if (altitude < CLOUD_SHELL_BASE || altitude >= CLOUD_TOP) return 0.0;
    vec4 wx = weather_state(normalize(p) * radius, radius);
    float coverage = clamp(wx.r, 0.0, 1.0);
    float storm = clamp(wx.g, 0.0, 1.0);
    float precip = clamp(wx.b, 0.0, 1.0);
    float lowp = low_pressure_weight(wx);

    float type = clamp(storm * 1.18 + precip * 0.10, 0.0, 1.0);
    float fair_cov = coverage_remap(coverage, type);
    float fair_profile = band_profile(altitude, mix(CLOUD_FAIR_BASE, 1000.0, type),
        fair_cloud_top(type), 180.0, mix(600.0, 1050.0, type));
    float fair_macro = worley_puff((p + wind) * CLOUD_SHAPE_SCALE);
    float fair = shaped_worley(fair_macro, fair_cov,
        mix(0.90, 0.97, smoothstep(0.08, 0.35, type))) * fair_profile;
    fair *= mix(0.50, 1.05, smoothstep(0.08, 0.34, type));

    float deep = smoothstep(0.42, 0.82,
        max(storm, precip * 0.70 + lowp * 0.22));
    float storm_cov = clamp(coverage * 0.72 + storm * 0.46 + lowp * 0.12, 0.0, 1.0);
    float core_macro = worley_puff((p + wind) * (CLOUD_SHAPE_SCALE * 0.70));
    float developed = shaped_worley(core_macro, storm_cov, 0.96)
        * band_profile(altitude, 1100.0, CLOUD_TOP, 220.0, 1200.0)
        * deep * (0.85 + 0.45 * storm);

    float haze = rain_haze_density(p, altitude, wx, wind);
    return (fair + developed + haze) * CLOUD_DENSITY;
}

float density_at(vec3 p, float radius, vec3 wind, float detail_weight) {
    return density_field(p, radius, wind, true, detail_weight);
}

float hg(float mu, float g) {
    float gg = g * g;
    return (1.0 - gg) / (4.0 * PI
        * pow(max(1.0 + gg - 2.0 * g * mu, 1e-4), 1.5));
}

// EVE Kerbin phaseFunctions:
// singleScattering1 = (0.95, 0.10), singleScattering2 = (0.8, 0.20).
// The exact forward spike is capped because Asterra evaluates one screen sample
// rather than EVE's full light-volume reconstruction.
float cloud_phase_single(float mu) {
    float p = (hg(mu, 0.95) * 0.10 + hg(mu, 0.80) * 0.20) / 0.30;
    return min(p, 3.5);
}

// EVE base-layer multiple-scattering lobes (0.2,3.0) and (-0.4,0.3).
float cloud_phase_multiple(float mu) {
    return (hg(mu, 0.20) * 3.0 + hg(mu, -0.40) * 0.30) / 3.30;
}

float planet_horizon_cosine(float sample_radius, float planet_radius) {
    float r = max(sample_radius, planet_radius + 0.01);
    float ratio = clamp(planet_radius / r, 0.0, 1.0);
    return -sqrt(max(1.0 - ratio * ratio, 0.0));
}

float planet_solar_clearance(vec3 p, vec3 sun_dir, float planet_radius) {
    float r = max(length(p), planet_radius + 0.01);
    vec3 up = p / r;
    float horizon_zenith = acos(clamp(
        planet_horizon_cosine(r, planet_radius), -1.0, 1.0));
    float sun_zenith = acos(clamp(dot(up, normalize(sun_dir)), -1.0, 1.0));
    return horizon_zenith - sun_zenith;
}

float helion_angular_radius() {
    return max(fract(params.wind_steps.w), 1e-7);
}

// Fraction of Helion's physical disc above the local curved-planet horizon.
float planet_sun_visibility(vec3 p, vec3 sun_dir, float planet_radius) {
    float clearance = planet_solar_clearance(p, sun_dir, planet_radius);
    float angular_radius = helion_angular_radius();
    if (clearance <= -angular_radius) return 0.0;
    if (clearance >= angular_radius) return 1.0;
    float x = clamp(clearance / angular_radius, -1.0, 1.0);
    float root = sqrt(max(1.0 - x * x, 0.0));
    return (acos(-x) + x * root) / PI;
}

float cloud_twilight_weight(float solar_clearance) {
    float rise = smoothstep(-0.035, 0.004, solar_clearance);
    float fall = 1.0 - smoothstep(0.018, 0.12, solar_clearance);
    return rise * fall;
}

float sun_transmittance(vec3 p, vec3 sun_dir, float radius,
        vec3 wind, int light_steps) {
    int steps = clamp(light_steps, 1, CLOUD_MAX_LIGHT_STEPS);
    float step_len = CLOUD_LIGHT_DISTANCE / float(steps);
    float optical_depth = 0.0;
    for (int j = 0; j < CLOUD_MAX_LIGHT_STEPS; j++) {
        if (j >= steps) break;
        // Kerbin's four-step light march is intentionally short/local.
        float f = (float(j) + 0.55) / float(steps);
        float shaped = f * f * 0.65 + f * 0.35;
        vec3 light_p = p + sun_dir * (shaped * CLOUD_LIGHT_DISTANCE);
        optical_depth += density_coarse(light_p, radius, wind) * step_len;
        if (optical_depth * CLOUD_EXTINCTION > 9.0) break;
    }
    return exp(-optical_depth * CLOUD_EXTINCTION * 0.82);
}

vec2 cloud_segment(vec3 origin, vec3 dir, float radius, float scene_distance) {
    float inner_radius = radius + CLOUD_SHELL_BASE;
    float outer_radius = radius + CLOUD_TOP;
    vec2 outer_hit = sphere_intersect(origin, dir, outer_radius);
    if (outer_hit.y <= 0.0) return vec2(1e30, -1e30);
    float camera_radius = length(origin);
    float ray_start = max(outer_hit.x, 0.0);
    float ray_end = min(outer_hit.y, scene_distance);
    vec2 inner_hit = sphere_intersect(origin, dir, inner_radius);
    if (camera_radius < inner_radius) {
        ray_start = max(ray_start, inner_hit.y);
    } else if (camera_radius < outer_radius) {
        ray_start = 0.0;
        if (inner_hit.x > 0.0 && inner_hit.x < ray_end) ray_end = inner_hit.x;
    } else if (inner_hit.x > ray_start && inner_hit.x < ray_end) {
        ray_end = inner_hit.x;
    }
    vec2 ground_hit = sphere_intersect(origin, dir, radius);
    if (ground_hit.x > 0.0) ray_end = min(ray_end, ground_hit.x);
    if (ray_end <= ray_start) return vec2(1e30, -1e30);
    return vec2(ray_start, ray_end);
}

// Preserve EVE's adaptive-step behavior while fitting Asterra's existing
// full-resolution compositor budget. The mapping covers the entire segment, but
// allocates more samples near the camera and wider samples toward the horizon.
float adaptive_distance(float f, float span) {
    float span_factor = max(CLOUD_ADAPTIVE_FACTOR * span / CLOUD_BASE_STEP, 0.0);
    float k = min(log(1.0 + span_factor), log(CLOUD_MAX_STEP / CLOUD_BASE_STEP));
    if (k < 1e-4) return f * span;
    return span * (exp(k * f) - 1.0) / max(exp(k) - 1.0, 1e-5);
}

vec4 raymarch_clouds(vec3 origin, vec3 dir, float radius, float scene_distance,
        vec3 sun_dir, float sun_irradiance, vec3 wind, int requested_steps,
        out float first_cloud_distance) {
    first_cloud_distance = -1.0;
    vec2 segment = cloud_segment(origin, dir, radius, scene_distance);
    if (segment.x > segment.y) return vec4(0.0, 0.0, 0.0, 1.0);

    int steps = clamp(requested_steps, 6, CLOUD_MAX_PRIMARY_STEPS);
    int light_steps = CLOUD_MAX_LIGHT_STEPS;
    float span = segment.y - segment.x;
    float jitter = hash13(dir * 173.0);
    float transmittance = 1.0;
    vec3 radiance = vec3(0.0);
    float mu = dot(dir, sun_dir);
    float phase_single = cloud_phase_single(mu);
    float phase_multiple = cloud_phase_multiple(mu);

    for (int i = 0; i < CLOUD_MAX_PRIMARY_STEPS; i++) {
        if (i >= steps || transmittance < 0.012) break;
        float f0 = float(i) / float(steps);
        float f1 = float(i + 1) / float(steps);
        float t0 = segment.x + adaptive_distance(f0, span);
        float t1 = segment.x + adaptive_distance(f1, span);
        float step_len = max(t1 - t0, 1.0);
        float t = mix(t0, t1, mix(0.28, 0.72, jitter));
        vec3 p = origin + dir * t;
        float detail_weight = 1.0 - smoothstep(42000.0, 115000.0, t);
        float density = density_at(p, radius, wind, detail_weight);
        if (density <= 0.006) continue;

        if (first_cloud_distance < 0.0) first_cloud_distance = t0;
        float sample_alpha = 1.0 - exp(-density * CLOUD_EXTINCTION * step_len);
        float planet_vis = planet_sun_visibility(p, sun_dir, radius);
        float solar_clearance = planet_solar_clearance(p, sun_dir, radius);
        float sun_air = mix(0.025, 1.0, smoothstep(-0.004, 0.28, solar_clearance));
        vec3 sunset_tint = mix(vec3(1.00, 0.34, 0.10),
            vec3(1.00, 0.98, 0.94),
            smoothstep(0.0, 0.24, solar_clearance));
        float light_trans = planet_vis > 0.001
            ? sun_transmittance(p, sun_dir, radius, wind, light_steps)
            : 0.0;

        // Beer-powder edge brightening from the Kerbin/Nubis path.
        float powder = 1.0 - exp(-density * CLOUD_EXTINCTION * step_len * 2.2);
        vec3 direct = sunset_tint * sun_irradiance * phase_single
            * light_trans * planet_vis * sun_air * mix(0.58, 1.03, powder);

        float day = smoothstep(-0.002, 0.18, solar_clearance) * planet_vis;
        float twilight = cloud_twilight_weight(solar_clearance);
        vec3 ambient = vec3(0.00018, 0.00030, 0.00065);
        ambient += vec3(0.0045, 0.0065, 0.0120) * twilight;
        ambient += vec3(0.050, 0.071, 0.102) * day; // Kerbin skylightMultiplier ~1

        // Analytic stand-in for EVE's time-sliced light volume. It uses the same
        // multiple-scattering phase pair but avoids a new 224^3 resource in this
        // first integration.
        float ms_energy = (0.010 + 0.032 * (1.0 - light_trans))
            * (0.55 + 0.45 * powder);
        vec3 multiple = sunset_tint * sun_irradiance * phase_multiple
            * ms_energy * planet_vis * day;

        radiance += transmittance * sample_alpha * (direct + ambient + multiple);
        transmittance *= 1.0 - sample_alpha;
    }
    return vec4(radiance, clamp(transmittance, 0.0, 1.0));
}

vec2 foreground_air_segment(vec3 camera_pos, vec3 dir, float first_t,
        float planet_radius) {
    vec2 hit = sphere_intersect(camera_pos, dir, planet_radius + ATMOSPHERE_TOP);
    if (hit.y <= 0.0) return vec2(1e30, -1e30);
    float ray_start = max(hit.x, 0.0);
    float ray_end = min(hit.y, first_t);
    if (ray_end <= ray_start) return vec2(1e30, -1e30);
    return vec2(ray_start, ray_end);
}

float foreground_air_transmittance(vec3 camera_pos, vec3 dir, float first_t,
        float planet_radius) {
    if (first_t <= 0.0) return 1.0;
    vec2 segment = foreground_air_segment(camera_pos, dir, first_t, planet_radius);
    if (segment.x > segment.y) return 1.0;
    float step_len = (segment.y - segment.x) / float(AIR_STEPS);
    float optical_length = 0.0;
    for (int i = 0; i < AIR_STEPS; i++) {
        float t = segment.x + (float(i) + 0.5) * step_len;
        float altitude = max(length(camera_pos + dir * t) - planet_radius, 0.0);
        float rayleigh = exp(-altitude / 8000.0);
        float mie = exp(-altitude / 1200.0);
        optical_length += (0.72 * rayleigh + 0.28 * mie) * step_len;
    }
    return exp(-optical_length * 1.55e-5);
}

vec3 foreground_atmosphere_restore(vec3 camera_pos, vec3 dir, float first_t,
        float cloud_transmittance, float air_transmittance,
        vec3 sun_dir, float planet_radius) {
    if (first_t <= 0.0 || cloud_transmittance > 0.999) return vec3(0.0);
    vec2 air_segment = foreground_air_segment(camera_pos, dir, first_t, planet_radius);
    if (air_segment.x > air_segment.y) return vec3(0.0);
    float air_mid_t = (air_segment.x + air_segment.y) * 0.5;
    vec3 mid_p = camera_pos + dir * air_mid_t;
    float solar_vis = planet_sun_visibility(mid_p, sun_dir, planet_radius);
    float solar_clearance = planet_solar_clearance(mid_p, sun_dir, planet_radius);
    float day = smoothstep(-0.002, 0.18, solar_clearance) * solar_vis;
    float twilight = cloud_twilight_weight(solar_clearance);
    vec3 local_up = normalize(mid_p);
    float elevation = clamp(dot(dir, local_up), -0.15, 1.0);
    vec3 day_haze = mix(vec3(0.30, 0.34, 0.38), vec3(0.19, 0.36, 0.52),
        smoothstep(-0.08, 0.25, elevation));
    vec3 dusk_haze = vec3(0.035, 0.013, 0.005);
    vec3 night_haze = vec3(0.00012, 0.00020, 0.00045);
    vec3 haze_color = night_haze + dusk_haze * twilight + day_haze * day;
    return haze_color * (1.0 - air_transmittance)
        * (1.0 - cloud_transmittance) * 0.55;
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(color_image);
    if (pixel.x >= size.x || pixel.y >= size.y) return;
    vec2 uv = (vec2(pixel) + vec2(0.5)) / vec2(size);
    float depth = textureLod(depth_texture, uv, 0.0).r;
    vec3 ndc = vec3(uv * 2.0 - 1.0, depth);
    vec4 world_h = params.inv_world_projection * vec4(ndc, 1.0);
    vec3 world_offset = world_h.xyz / max(abs(world_h.w), 1e-8) * sign(world_h.w);
    float scene_distance = depth <= 1e-6 ? 1e30 : length(world_offset);
    if (!(scene_distance > 0.0)) return;
    vec3 ray_world = normalize(world_offset);
    vec3 camera_planet = params.camera_planet_radius.xyz;
    float planet_radius = params.camera_planet_radius.w;
    vec3 sun_dir = normalize(params.sun_dir_intensity.xyz);
    float sun_irradiance = params.sun_dir_intensity.w;
    vec3 wind = params.wind_steps.xyz;
    int steps = int(clamp(floor(params.wind_steps.w), 6.0,
        float(CLOUD_MAX_PRIMARY_STEPS)));

    float first_cloud_distance;
    vec4 cloud = raymarch_clouds(camera_planet, ray_world, planet_radius,
        max(scene_distance - 0.5, 0.0), sun_dir, sun_irradiance, wind, steps,
        first_cloud_distance);
    if (cloud.a > 0.9999) return;

    vec4 base = imageLoad(color_image, pixel);
    float air_t = foreground_air_transmittance(camera_planet, ray_world,
        first_cloud_distance, planet_radius);
    vec3 result = cloud.rgb * air_t + base.rgb * cloud.a;
    result += foreground_atmosphere_restore(camera_planet, ray_world,
        first_cloud_distance, cloud.a, air_t, sun_dir, planet_radius);
    imageStore(color_image, pixel, vec4(result, base.a));
}
