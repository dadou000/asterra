#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(rgba16f, set = 0, binding = 0) uniform image2D color_image;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 0, binding = 2) uniform sampler3D shape_noise;
layout(set = 0, binding = 3) uniform sampler3D detail_noise;
layout(set = 0, binding = 4) uniform sampler2D global_weather;
layout(set = 0, binding = 5) uniform sampler2D local_weather;
layout(set = 0, binding = 6) uniform sampler3D light_volume;
layout(set = 0, binding = 7) uniform sampler2D lightning_events;

layout(push_constant, std430) uniform Params {
    vec4 camera_planet_radius;
    vec4 sun_dir_intensity;
    vec4 wind_steps;
    // xyz local weather/light-volume centre. abs(w)=span; w<0 means volume warmup.
    vec4 weather_center_span;
    mat4 inv_world_projection;
} params;

const float PI = 3.14159265358979323846;
const float ATMOSPHERE_TOP = 60000.0;
const float CLOUD_TOP = 14500.0;
const float CLOUD_SHAPE_SCALE = 0.0000520;
const float CLOUD_DETAIL_SCALE = 0.00042;
const float CLOUD_EXTINCTION = 0.0010;
const float WORLEY_PERSISTENCE = 0.57;

const float BASE_STEP = 90.0;
const float ADAPTIVE_FACTOR = 0.000025;
const float MAX_STEP = 650.0;
const float EMPTY_STEP_NEAR = 2200.0;
const float EMPTY_STEP_FAR = 6200.0;
const float LIGHT_DISTANCE = 16000.0;
const float ORBIT_FADE_START = 70000.0;
const float ORBIT_FADE_END = 260000.0;
const int MAX_PRIMARY_STEPS = 96;
const int MAX_EMPTY_PROBES = 64;
const int MAX_LIGHT_STEPS = 10;
const int LIGHTNING_STEPS = 5;
const int MAX_LIGHTNING_EVENTS = 4;
const int GODRAY_STEPS = 6;
const int AIR_STEPS = 4;

float saturate1(float x) { return clamp(x, 0.0, 1.0); }

vec2 sphere_hit(vec3 o, vec3 d, float r) {
    float b = dot(d, o);
    float c = dot(o, o) - r * r;
    float q = b * b - c;
    if (q < 0.0) return vec2(1e30, -1e30);
    float s = sqrt(q);
    return vec2(-b - s, -b + s);
}

vec2 global_uv(vec3 d) {
    d = normalize(d);
    float lon = atan(d.z, d.x);
    if (lon < 0.0) lon += 2.0 * PI;
    float lat = asin(clamp(d.y, -1.0, 1.0));
    return vec2(lon / (2.0 * PI), (PI * 0.5 - lat) / PI);
}

void local_basis(out vec3 center, out vec3 east, out vec3 north) {
    center = normalize(params.weather_center_span.xyz);
    vec3 pole = abs(center.y) > 0.92 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    east = normalize(cross(pole, center));
    north = normalize(cross(center, east));
}

bool uv_inside_01(vec2 uv) {
    return all(greaterThanEqual(uv, vec2(0.0)))
        && all(lessThanEqual(uv, vec2(1.0)));
}

vec4 weather_state(vec3 d, float radius) {
    d = normalize(d);
    vec4 g = textureLod(global_weather, global_uv(d), 0.0);
    vec3 center, east, north;
    local_basis(center, east, north);
    vec3 delta = d * radius - center * radius;
    float span = max(abs(params.weather_center_span.w), 1000.0);
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
    vec4 wx = weather_state(radial, radius);
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

    // Coverage is treated as an area fraction, not a density multiplier. A low
    // frequency synoptic mask creates genuine holes in weather systems so medium
    // coverage cannot become a planet-wide white carpet.
    float coverage_target = smoothstep(0.08, 0.94, coverage);
    float synoptic_noise = cloud_fbm(warped_surface,
        CLOUD_SHAPE_SCALE * 0.075, wind * 0.16);
    float synoptic_threshold = mix(0.82, 0.46, coverage_target)
        - convection * 0.075;
    float synoptic = smoothstep(synoptic_threshold,
        min(synoptic_threshold + 0.095, 0.995), synoptic_noise);
    // footprint/tower/anvil below are each multiplied by synoptic, so synoptic ==
    // 0 forces the final density to exactly 0 no matter what the remaining five
    // noise octaves evaluate to -- an exact equivalence, not an approximation.
    // This function runs as the empty-space probe in detailed_clouds() up to
    // MAX_EMPTY_PROBES times per pixel, so skipping them here (the common case
    // over clear sky, by design -- see the coverage comment above) is the
    // dominant cost of a cloudless view.
    if (synoptic <= 0.0) return 0.0;

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

    // The tower and anvil remain connected to the same footprint. No altitude
    // sheet is introduced; vertical growth is a deformation of one cloud body.
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

float detailed_density(vec3 p, float radius, vec3 wind, float detail_weight) {
    float coarse = canonical_coarse_density(p, radius, wind);
    if (coarse <= 0.002 || detail_weight <= 0.001) return coarse;

    vec3 uv = (p + wind * 1.31) * CLOUD_DETAIL_SCALE;
    float d0 = textureLod(detail_noise,
        uv + vec3(0.41, 0.17, 0.83), 0.0).r;
    float d1 = textureLod(detail_noise,
        uv * 1.93 + vec3(0.07, 0.67, 0.29), 0.0).r;
    float detail = (d0 + 0.55 * d1) / 1.55;
    float boundary = 1.0 - smoothstep(0.28, 0.84, coarse);
    float erosion = (1.0 - detail) * 0.30 * boundary * detail_weight;
    return max(coarse - erosion, 0.0);
}

float hg(float mu, float g) {
    float gg = g * g;
    return (1.0 - gg)
        / (4.0 * PI * pow(max(1.0 + gg - 2.0 * g * mu, 1e-4), 1.5));
}

float phase_single(float mu) {
    return hg(mu, 0.78) * 0.82 + hg(mu, -0.20) * 0.18;
}

float phase_multiple(float mu) {
    return hg(mu, 0.25) * 1.45 + hg(mu, -0.35) * 0.50;
}

float planet_sun_visibility(vec3 p, vec3 sun_dir, float radius) {
    vec2 h = sphere_hit(p, sun_dir, radius);
    return h.y > 0.0 && h.x > 0.001 ? 0.0 : 1.0;
}

float sun_transmittance(vec3 p, vec3 sun_dir, float radius,
        vec3 wind, int steps) {
    steps = clamp(steps, 1, MAX_LIGHT_STEPS);
    float optical_depth = 0.0;
    float previous_distance = 0.0;
    for (int i = 0; i < MAX_LIGHT_STEPS; ++i) {
        if (i >= steps) break;
        float f = float(i + 1) / float(steps);
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

float lightning_transmittance(vec3 p, vec3 light_pos,
        float radius, vec3 wind) {
    vec3 to_light = light_pos - p;
    float distance_m = length(to_light);
    if (distance_m < 1.0) return 1.0;
    vec3 dir = to_light / distance_m;
    float dl = distance_m / float(LIGHTNING_STEPS);
    float optical_depth = 0.0;
    for (int i = 0; i < LIGHTNING_STEPS; ++i) {
        float t = (float(i) + 0.5) * dl;
        optical_depth += canonical_coarse_density(p + dir * t,
            radius, wind) * dl;
        if (optical_depth * CLOUD_EXTINCTION > 8.0) break;
    }
    return exp(-optical_depth * CLOUD_EXTINCTION * 0.72);
}

vec3 lightning_scattering(vec3 p, float density,
        float radius, vec3 wind) {
    vec3 result = vec3(0.0);
    for (int i = 0; i < MAX_LIGHTNING_EVENTS; ++i) {
        vec4 event = texelFetch(lightning_events, ivec2(i, 0), 0);
        float strength = event.w;
        if (strength <= 0.001) continue;
        vec3 delta = event.xyz - p;
        float distance_m = length(delta);
        float influence_radius = mix(2800.0, 8500.0, sqrt(strength));
        if (distance_m >= influence_radius) continue;
        float x = distance_m / influence_radius;
        float falloff = exp(-x * x * 3.1) * (1.0 - smoothstep(0.78, 1.0, x));
        if (falloff <= 0.001) continue;
        float transmission = lightning_transmittance(
            p, event.xyz, radius, wind);
        float cloud_water = 1.0 - exp(-density * 1.8);
        result += vec3(0.76, 0.87, 1.0) * strength * falloff
            * transmission * mix(3.8, 9.0, cloud_water);
    }
    return result;
}

float orbital_lightning(vec3 shell_p) {
    float glow = 0.0;
    for (int i = 0; i < MAX_LIGHTNING_EVENTS; ++i) {
        vec4 event = texelFetch(lightning_events, ivec2(i, 0), 0);
        if (event.w <= 0.001) continue;
        vec3 projected = normalize(event.xyz) * length(shell_p);
        float distance_m = length(projected - shell_p);
        float r = mix(10000.0, 26000.0, sqrt(event.w));
        float x = distance_m / r;
        glow += event.w * exp(-x * x * 3.0);
    }
    return glow;
}

bool sample_light_volume(vec3 p, float radius, out vec4 lv) {
    lv = vec4(1.0, 0.0, 0.05, 0.0);
    if (params.weather_center_span.w >= 0.0) {
        vec3 center, east, north;
        local_basis(center, east, north);
        vec3 d = normalize(p);
        vec3 delta = d * radius - center * radius;
        float span = max(abs(params.weather_center_span.w), 1000.0);
        vec2 uv = vec2(dot(delta, east), dot(delta, north)) / span + vec2(0.5);
        float alt = length(p) - radius;
        vec3 uvw = vec3(uv, alt / CLOUD_TOP);
        if (all(greaterThanEqual(uvw, vec3(0.0)))
                && all(lessThanEqual(uvw, vec3(1.0)))) {
            lv = textureLod(light_volume, uvw, 0.0);
            return true;
        }
    }
    return false;
}

vec2 cloud_segment(vec3 o, vec3 d, float radius, float scene_dist) {
    vec2 outer = sphere_hit(o, d, radius + CLOUD_TOP);
    if (outer.y <= 0.0) return vec2(1e30, -1e30);
    float start = max(outer.x, 0.0);
    float end = min(outer.y, scene_dist);
    vec2 ground = sphere_hit(o, d, radius);
    if (ground.x > 0.0) end = min(end, ground.x);
    if (end <= start) return vec2(1e30, -1e30);
    return vec2(start, end);
}

float interleaved_gradient_noise(vec2 pixel) {
    return fract(52.9829189 * fract(0.06711056 * pixel.x
        + 0.00583715 * pixel.y));
}

vec4 detailed_clouds(vec3 o, vec3 d, float radius, float scene_dist,
        vec3 sun_dir, float irradiance, vec3 wind, int requested_steps,
        float camera_alt, vec2 pixel, out float first_t) {
    first_t = -1.0;
    vec2 seg = cloud_segment(o, d, radius, scene_dist);
    if (seg.x > seg.y) return vec4(0.0, 0.0, 0.0, 1.0);

    int cloud_budget = clamp(requested_steps, 12, MAX_PRIMARY_STEPS);
    int cloud_used = 0;
    int probes_used = 0;
    float jitter = interleaved_gradient_noise(pixel);
    float t = seg.x + mix(0.05, 0.55, jitter) * min(EMPTY_STEP_NEAR, seg.y - seg.x);
    float trans = 1.0;
    vec3 radiance = vec3(0.0);
    float mu = dot(d, sun_dir);
    float ps = phase_single(mu);
    float pm = phase_multiple(mu);
    bool previous_was_empty = true;
    float previous_empty_step = EMPTY_STEP_NEAR;

    while (t < seg.y && trans > 0.008
            && cloud_used < cloud_budget
            && probes_used < MAX_EMPTY_PROBES) {
        vec3 probe_p = o + d * t;
        float coarse = canonical_coarse_density(probe_p, radius, wind);

        if (coarse < 0.002) {
            float empty_step = mix(EMPTY_STEP_NEAR, EMPTY_STEP_FAR,
                smoothstep(12000.0, 190000.0, t));
            empty_step *= mix(0.82, 1.18, jitter);
            previous_empty_step = empty_step;
            t += min(empty_step, seg.y - t);
            probes_used++;
            previous_was_empty = true;
            continue;
        }

        // Refine the first occupied point after a coarse jump. This removes the
        // camera-centred shells/rings produced when a long empty step lands at
        // different altitudes on neighbouring rays.
        if (previous_was_empty) {
            float lo = max(seg.x, t - previous_empty_step);
            float hi = t;
            for (int r = 0; r < 4; ++r) {
                float mid = 0.5 * (lo + hi);
                float md = canonical_coarse_density(o + d * mid, radius, wind);
                if (md >= 0.002) hi = mid;
                else lo = mid;
            }
            t = hi;
            previous_was_empty = false;
            continue;
        }

        float step_len = min(BASE_STEP * (1.0 + t * ADAPTIVE_FACTOR), MAX_STEP);
        step_len = min(step_len, seg.y - t);
        vec3 p = o + d * (t + 0.5 * step_len);

        float camera_detail = 1.0 - smoothstep(22000.0, 85000.0, camera_alt);
        float distance_detail = 1.0 - smoothstep(12000.0, 65000.0, t);
        float detail_weight = camera_detail * distance_detail;
        float den = detailed_density(p, radius, wind, detail_weight);

        if (den > 0.004) {
            if (first_t < 0.0) first_t = t;
            float alpha = 1.0 - exp(-den * CLOUD_EXTINCTION * step_len);
            float planet_vis = planet_sun_visibility(p, sun_dir, radius);

            vec4 lv;
            bool have_lv = sample_light_volume(p, radius, lv);
            int light_steps = clamp(4 + cloud_budget / 16, 4, MAX_LIGHT_STEPS);
            float direct_t = have_lv
                ? lv.r * planet_vis
                : sun_transmittance(p, sun_dir, radius, wind, light_steps) * planet_vis;

            float powder = 1.0 - exp(-den * CLOUD_EXTINCTION * step_len * 2.4);
            float boundary = 1.0 - smoothstep(0.30, 0.86, den);
            float silver = pow(max(mu, 0.0), 12.0) * boundary * direct_t;
            float sun_elev = dot(normalize(p), sun_dir);
            vec3 sunlight_tint = mix(vec3(1.0, 0.32, 0.085),
                vec3(1.0, 0.985, 0.96), smoothstep(-0.025, 0.30, sun_elev));
            float day = smoothstep(-0.12, 0.15, sun_elev) * planet_vis;

            vec3 direct = sunlight_tint * irradiance * ps * 2.15 * direct_t
                * mix(0.62, 1.02, powder) * (1.0 + silver * 1.85);

            float multiple_reservoir;
            float skylight;
            if (have_lv) {
                multiple_reservoir = lv.g;
                skylight = lv.b;
            } else {
                float optical = -log(max(direct_t, 1e-4));
                float order2 = exp(-optical * 0.48);
                float order3 = exp(-optical * 0.20);
                multiple_reservoir = (0.020 + 0.055 * order2 + 0.024 * order3)
                    * (0.30 + 0.70 * powder);
                skylight = mix(0.012, 0.075, day) * mix(1.0, 0.45, den);
            }
            vec3 multiple = sunlight_tint * irradiance * pm
                * multiple_reservoir * day;
            vec3 ambient = vec3(0.43, 0.61, 0.92) * skylight
                * mix(0.20, 1.0, day);
            vec3 lightning = lightning_scattering(p, den, radius, wind);

            radiance += trans * alpha * (direct + multiple + ambient + lightning);
            trans *= 1.0 - alpha;
        }

        t += step_len;
        cloud_used++;
    }

    return vec4(radiance, clamp(trans, 0.0, 1.0));
}

float orbital_coverage(vec3 radial, float radius, vec3 wind,
        out float storm_amount) {
    vec4 wx = weather_state(radial, radius);
    float coverage = saturate1(wx.r);
    float storm = saturate1(wx.g);
    float precip = saturate1(wx.b);
    float low_pressure = saturate1((0.5 - wx.a) * 3.0);
    float convection = smoothstep(0.12, 0.86,
        max(storm, precip * 0.76 + low_pressure * 0.20));
    storm_amount = convection;

    vec3 surface_p = radial * radius;
    vec3 warped = surface_p + cloud_domain_warp(surface_p, wind, convection) * 0.45;
    float target = smoothstep(0.08, 0.94, coverage);
    float macro = cloud_fbm(warped, CLOUD_SHAPE_SCALE * 0.055, wind * 0.13);
    float threshold = mix(0.82, 0.46, target) - convection * 0.075;
    float body = smoothstep(threshold, min(threshold + 0.10, 0.995), macro);
    float modulation = cloud_fbm(warped,
        CLOUD_SHAPE_SCALE * 0.12, wind * 0.21);
    body *= mix(0.72, 1.0, smoothstep(0.38, 0.68, modulation));
    return saturate1(body * mix(0.72, 1.08, convection));
}

vec4 orbital_clouds(vec3 o, vec3 d, float radius, float scene_dist,
        vec3 sun_dir, float irradiance, vec3 wind) {
    float shell = radius + 6500.0;
    vec2 h = sphere_hit(o, d, shell);
    float t = h.x > 0.0 ? h.x : h.y;
    if (t <= 0.0 || t >= scene_dist)
        return vec4(0.0, 0.0, 0.0, 1.0);

    vec3 shell_p = o + d * t;
    vec3 radial = normalize(shell_p);
    float storm_amount = 0.0;
    float body = orbital_coverage(radial, radius, wind, storm_amount);
    float alpha = 1.0 - exp(-body * mix(0.95, 1.45, storm_amount));
    alpha = clamp(alpha, 0.0, 0.93);
    if (alpha < 0.003) return vec4(0.0, 0.0, 0.0, 1.0);

    float ndl = max(dot(radial, sun_dir), 0.0);
    vec3 c = mix(vec3(0.055, 0.075, 0.12),
        vec3(0.72, 0.79, 0.88), sqrt(ndl)) * irradiance * 0.12;
    float flash = orbital_lightning(shell_p);
    c += vec3(0.72, 0.86, 1.0) * flash * 1.45;
    return vec4(c * alpha, 1.0 - alpha);
}

vec3 god_rays(vec3 o, vec3 d, float radius, float max_t,
        vec3 sun_dir, float irradiance) {
    if (params.weather_center_span.w < 0.0) return vec3(0.0);
    float camera_alt = length(o) - radius;
    if (camera_alt > ATMOSPHERE_TOP) return vec3(0.0);
    float forward = pow(max(dot(d, sun_dir), 0.0), 8.0);
    if (forward < 0.002) return vec3(0.0);
    vec2 ah = sphere_hit(o, d, radius + ATMOSPHERE_TOP);
    float end = min(max_t, ah.y);
    if (end <= 0.0) return vec3(0.0);
    end = min(end, 50000.0);
    float dl = end / float(GODRAY_STEPS);
    float accum = 0.0;
    for (int i = 0; i < GODRAY_STEPS; ++i) {
        float t = (float(i) + 0.5) * dl;
        vec3 p = o + d * t;
        float alt = max(length(p) - radius, 0.0);
        float air = exp(-alt / 8000.0) * 0.72
            + exp(-alt / 1200.0) * 0.28;
        vec4 lv;
        if (sample_light_volume(p, radius, lv))
            accum += air * lv.r * dl;
    }
    float strength = (1.0 - exp(-accum * 2.2e-5)) * forward;
    return vec3(1.0, 0.86, 0.67) * strength * irradiance * 0.035;
}

float foreground_air_t(vec3 o, vec3 d, float first_t, float radius) {
    if (first_t <= 0.0) return 1.0;
    vec2 h = sphere_hit(o, d, radius + ATMOSPHERE_TOP);
    float start = max(h.x, 0.0);
    float end = min(h.y, first_t);
    if (end <= start) return 1.0;
    float dl = (end - start) / float(AIR_STEPS);
    float od = 0.0;
    for (int i = 0; i < AIR_STEPS; ++i) {
        float t = start + (float(i) + 0.5) * dl;
        float alt = max(length(o + d * t) - radius, 0.0);
        od += (0.72 * exp(-alt / 8000.0)
            + 0.28 * exp(-alt / 1200.0)) * dl;
    }
    return exp(-od * 1.55e-5);
}

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(color_image);
    if (any(greaterThanEqual(pixel, size))) return;

    vec2 uv = (vec2(pixel) + vec2(0.5)) / vec2(size);
    float depth = textureLod(depth_texture, uv, 0.0).r;
    vec3 ndc = vec3(uv * 2.0 - 1.0, depth);
    vec4 wh = params.inv_world_projection * vec4(ndc, 1.0);
    vec3 world_offset = wh.xyz / max(abs(wh.w), 1e-8) * sign(wh.w);
    float scene_dist = depth <= 1e-6 ? 1e30 : length(world_offset);
    if (!(scene_dist > 0.0)) return;

    vec3 ray = normalize(world_offset);
    vec3 camera = params.camera_planet_radius.xyz;
    float radius = params.camera_planet_radius.w;
    vec3 sun = normalize(params.sun_dir_intensity.xyz);
    float irradiance = params.sun_dir_intensity.w;
    vec3 wind = params.wind_steps.xyz;
    int steps = int(clamp(floor(params.wind_steps.w),
        12.0, float(MAX_PRIMARY_STEPS)));
    float camera_alt = length(camera) - radius;
    float orbital_w = smoothstep(ORBIT_FADE_START, ORBIT_FADE_END, camera_alt);

    float first_t = -1.0;
    vec4 detailed = vec4(0.0, 0.0, 0.0, 1.0);
    if (orbital_w < 0.999) {
        detailed = detailed_clouds(camera, ray, radius,
            max(scene_dist - 0.5, 0.0), sun, irradiance, wind, steps,
            camera_alt, vec2(pixel), first_t);
    }
    vec4 orbital = vec4(0.0, 0.0, 0.0, 1.0);
    if (orbital_w > 0.001) {
        orbital = orbital_clouds(camera, ray, radius,
            max(scene_dist - 0.5, 0.0), sun, irradiance, wind);
    }
    vec4 cloud = mix(detailed, orbital, orbital_w);

    vec4 base = imageLoad(color_image, pixel);
    float air_t = foreground_air_t(camera, ray, first_t, radius);
    vec3 result = cloud.rgb * air_t + base.rgb * cloud.a;
    float ray_end = first_t > 0.0 ? first_t : min(scene_dist, 50000.0);
    result += god_rays(camera, ray, radius, ray_end, sun, irradiance)
        * (1.0 - cloud.a * 0.45);
    imageStore(color_image, pixel, vec4(result, base.a));
}
