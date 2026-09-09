/// Procedural fallback for Asterra's EVE/Kerbin-derived volumetric clouds.
///
/// The depth-aware compositor is the normal renderer. This file is used only when
/// the RenderingDevice compositor cannot initialize, but it shares the same
/// cellular/Worley primitive, Kerbin persistence/erosion and optical tuning so a
/// fallback does not revert to the older unrelated synthetic cloud look.

uniform sampler3D u_cloud_shape_noise : repeat_enable, filter_linear;
uniform sampler3D u_cloud_detail_noise : repeat_enable, filter_linear;

uniform float u_cloud_enabled = 1.0;
uniform float u_cloud_base = 800.0;
uniform float u_cloud_top = 14500.0;
uniform float u_cloud_coverage = 0.28;
uniform float u_cloud_weather_variation = 0.30;
uniform float u_cloud_density = 1.0;
uniform float u_cloud_shape_scale = 0.000052;
uniform float u_cloud_detail_scale = 0.00042;
uniform float u_cloud_weather_scale = 0.0000085;
uniform float u_cloud_detail_strength = 0.65;
uniform float u_cloud_extinction = 0.0010;
uniform vec3 u_cloud_wind_offset = vec3(0.0);
uniform int u_cloud_primary_steps = 40;
uniform int u_cloud_light_steps = 4;

const float CLOUD_WORLEY_PERSISTENCE = 0.57;
const float CLOUD_LIGHT_DISTANCE = 2000.0;
const float CLOUD_UV_WARP_METRES = 200.0;
const int CLOUD_MAX_PRIMARY_STEPS = 64;
const int CLOUD_MAX_LIGHT_STEPS = 8;

vec2 cloud_sphere_intersect(vec3 origin, vec3 dir, float radius) {
	float b = dot(dir, origin);
	float c = dot(origin, origin) - radius * radius;
	float disc = b * b - c;
	if (disc < 0.0) return vec2(1e9, -1e9);
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
}

vec2 cloud_layer_segment(vec3 origin, vec3 dir) {
	float inner_radius = u_planet_radius + u_cloud_base;
	float outer_radius = u_planet_radius + u_cloud_top;
	vec2 outer_hit = cloud_sphere_intersect(origin, dir, outer_radius);
	if (outer_hit.y <= 0.0) return vec2(1e9, -1e9);

	float camera_radius = length(origin);
	float ray_start = max(outer_hit.x, 0.0);
	float ray_end = outer_hit.y;
	vec2 inner_hit = cloud_sphere_intersect(origin, dir, inner_radius);

	if (camera_radius < inner_radius) {
		ray_start = max(ray_start, inner_hit.y);
	} else if (camera_radius < outer_radius) {
		ray_start = 0.0;
		if (inner_hit.x > 0.0 && inner_hit.x < ray_end) {
			ray_end = inner_hit.x;
		}
	} else if (inner_hit.x > ray_start && inner_hit.x < ray_end) {
		ray_end = inner_hit.x;
	}

	vec2 ground_hit = cloud_sphere_intersect(origin, dir, u_planet_radius);
	if (ground_hit.x > 0.0) {
		ray_end = min(ray_end, ground_hit.x);
	}
	if (ray_end <= ray_start) return vec2(1e9, -1e9);
	return vec2(ray_start, ray_end);
}

float cloud_remap01(float v, float a, float b) {
	return clamp((v - a) / max(b - a, 1e-5), 0.0, 1.0);
}

float cloud_worley_puff(vec3 uv) {
	return 1.0 - texture(u_cloud_shape_noise, uv).r;
}

float cloud_worley_fbm(vec3 p, float scale) {
	vec3 uv = (p + u_cloud_wind_offset) * scale;
	float n0 = cloud_worley_puff(uv);
	float n1 = cloud_worley_puff(uv * 2.03 + vec3(0.19, 0.61, 0.43));
	return (n0 + CLOUD_WORLEY_PERSISTENCE * n1) / (1.0 + CLOUD_WORLEY_PERSISTENCE);
}

vec3 cloud_domain_warp(vec3 surface_p) {
	vec3 uv = (surface_p + u_cloud_wind_offset * 0.16)
		* (u_cloud_shape_scale * 0.12);
	vec3 q = vec3(
		texture(u_cloud_shape_noise, uv + vec3(0.13, 0.47, 0.81)).r,
		texture(u_cloud_shape_noise, uv + vec3(0.71, 0.23, 0.37)).r,
		texture(u_cloud_shape_noise, uv + vec3(0.41, 0.89, 0.17)).r);
	return (q * 2.0 - 1.0) * CLOUD_UV_WARP_METRES;
}

float cloud_synthetic_weather(vec3 surface_p) {
	// Only for fallback. The normal compositor samples WeatherSystem directly.
	float a = cloud_worley_puff(
		(surface_p + u_cloud_wind_offset * 0.16) * u_cloud_weather_scale);
	float b = cloud_worley_puff(
		(surface_p + u_cloud_wind_offset * 0.09)
		* (u_cloud_weather_scale * 0.53) + vec3(0.37, 0.11, 0.71));
	return mix(a, b, 0.32);
}

float cloud_coverage_remap(float coverage, float convective) {
	float stratus_curve = smoothstep(0.035, 0.72, coverage);
	float cumulus_curve = smoothstep(0.020, 0.58, coverage);
	return mix(stratus_curve, cumulus_curve, clamp(convective, 0.0, 1.0));
}

float cloud_edge_width(float hardness) {
	return mix(0.23, 0.045, clamp(hardness, 0.0, 1.0));
}

float cloud_shape_from_worley(float macro, float coverage, float hardness) {
	float threshold = 1.0 - clamp(coverage, 0.0, 1.0) * 0.76;
	float width = cloud_edge_width(hardness);
	return smoothstep(threshold, min(threshold + width, 0.999), macro);
}

float cloud_band(float altitude, float base_alt, float top_alt,
		float bottom_soft, float top_soft) {
	if (altitude <= base_alt || altitude >= top_alt) return 0.0;
	float bottom = smoothstep(base_alt, base_alt + bottom_soft, altitude);
	float top = 1.0 - smoothstep(top_alt - top_soft, top_alt, altitude);
	return bottom * top;
}

float cloud_fair_top(float t) {
	if (t < 0.20) return mix(3450.0, 4450.0, t / 0.20);
	if (t < 0.40) return mix(4450.0, 5450.0, (t - 0.20) / 0.20);
	if (t < 0.60) return mix(5450.0, 6450.0, (t - 0.40) / 0.20);
	if (t < 0.80) return mix(6450.0, 7450.0, (t - 0.60) / 0.20);
	return mix(7450.0, 9300.0, (t - 0.80) / 0.20);
}

float cloud_density_core(vec3 p, bool with_detail, float detail_weight) {
	float altitude = length(p) - u_planet_radius;
	if (altitude <= u_cloud_base || altitude >= u_cloud_top) return 0.0;

	vec3 surface_p = normalize(p) * u_planet_radius;
	float weather = cloud_synthetic_weather(surface_p);
	float coverage = clamp(
		u_cloud_coverage + (weather - 0.5) * u_cloud_weather_variation,
		0.02, 0.96);
	float type = smoothstep(0.30, 0.82, weather);
	float effective_coverage = cloud_coverage_remap(coverage, type);
	float hardness = mix(0.90, 0.97, smoothstep(0.08, 0.35, type));
	float profile = cloud_band(altitude, mix(1100.0, 1000.0, type),
		cloud_fair_top(type), 180.0, mix(600.0, 1050.0, type));

	vec3 warped_p = p + cloud_domain_warp(surface_p);
	float macro = cloud_worley_fbm(warped_p, u_cloud_shape_scale);
	float body = cloud_shape_from_worley(
		macro, effective_coverage, hardness) * profile;

	if (with_detail && detail_weight > 0.001 && body > 0.006) {
		vec3 detail_p = (p + u_cloud_wind_offset * 1.31) * u_cloud_detail_scale;
		float detail = texture(u_cloud_detail_noise,
			detail_p + vec3(0.41, 0.17, 0.83)).r;
		float edge = 1.0 - smoothstep(0.24, 0.88, body);
		body = max(body - (1.0 - detail) * u_cloud_detail_strength
			* detail_weight * (0.18 + 0.82 * edge) * (1.0 - body), 0.0);
	}

	float optical_scale = mix(0.50, 1.05, smoothstep(0.08, 0.34, type));
	return smoothstep(0.006, 0.24, body) * optical_scale * u_cloud_density;
}

float cloud_density_coarse(vec3 p) {
	return cloud_density_core(p, false, 0.0);
}

float cloud_density_at(vec3 p, float detail_weight) {
	return cloud_density_core(p, true, detail_weight);
}

float cloud_hg(float mu, float g) {
	float gg = g * g;
	return (1.0 - gg) / (4.0 * PI
		* pow(max(1.0 + gg - 2.0 * g * mu, 1e-4), 1.5));
}

float cloud_phase_single(float mu) {
	float p = (cloud_hg(mu, 0.95) * 0.10
		+ cloud_hg(mu, 0.80) * 0.20) / 0.30;
	return min(p, 3.5);
}

float cloud_phase_multiple(float mu) {
	return (cloud_hg(mu, 0.20) * 3.0
		+ cloud_hg(mu, -0.40) * 0.30) / 3.30;
}

float cloud_sun_transmittance(vec3 p, vec3 sun_dir, int requested_steps) {
	int light_steps = clamp(requested_steps, 1, CLOUD_MAX_LIGHT_STEPS);
	float step_len = CLOUD_LIGHT_DISTANCE / float(light_steps);
	float optical_depth = 0.0;

	for (int j = 0; j < CLOUD_MAX_LIGHT_STEPS; j++) {
		if (j >= light_steps) break;
		float f = (float(j) + 0.55) / float(light_steps);
		float shaped = f * f * 0.65 + f * 0.35;
		vec3 light_p = p + sun_dir * (shaped * CLOUD_LIGHT_DISTANCE);
		optical_depth += cloud_density_coarse(light_p) * step_len;
		if (optical_depth * u_cloud_extinction > 9.0) break;
	}
	return exp(-optical_depth * u_cloud_extinction * 0.82);
}

float cloud_ray_jitter(vec3 d) {
	return fract(sin(dot(d, vec3(91.17, 37.53, 141.73))) * 43758.5453);
}

/// RGB = accumulated cloud radiance. A = background transmittance.
vec4 raymarch_clouds(
	vec3 origin,
	vec3 dir,
	vec3 sun_dir,
	vec3 sun_color,
	float sun_irradiance,
	int requested_steps,
	int requested_light_steps
) {
	if (AT_CUBEMAP_PASS || u_cloud_enabled < 0.5) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}
	vec2 segment = cloud_layer_segment(origin, dir);
	if (segment.x > segment.y) return vec4(0.0, 0.0, 0.0, 1.0);

	int steps = clamp(requested_steps, 4, CLOUD_MAX_PRIMARY_STEPS);
	float step_len = (segment.y - segment.x) / float(steps);
	float jitter = cloud_ray_jitter(dir);
	float t = segment.x + step_len * mix(0.18, 0.82, jitter);
	float transmittance = 1.0;
	vec3 radiance = vec3(0.0);
	float mu = dot(dir, sun_dir);
	float phase_single = cloud_phase_single(mu);
	float phase_multiple = cloud_phase_multiple(mu);

	for (int i = 0; i < CLOUD_MAX_PRIMARY_STEPS; i++) {
		if (i >= steps || transmittance < 0.012 || t >= segment.y) break;
		vec3 p = origin + dir * t;
		float detail_weight = 1.0 - smoothstep(42000.0, 115000.0, t);
		float density = cloud_density_at(p, detail_weight);
		if (density > 0.006) {
			float sample_alpha = 1.0 - exp(-density * u_cloud_extinction * step_len);
			vec3 up = normalize(p);
			float sun_mu = dot(up, sun_dir);
			float planet_vis = planet_direct_sun_visibility(p, sun_dir, u_planet_radius);
			float sun_air = mix(0.16, 1.0, smoothstep(-0.015, 0.32, sun_mu));
			vec3 sunset_tint = mix(
				vec3(1.00, 0.34, 0.10),
				vec3(1.00, 0.98, 0.94),
				smoothstep(-0.02, 0.28, sun_mu));
			float light_trans = cloud_sun_transmittance(
				p, sun_dir, requested_light_steps);

			float powder = 1.0 - exp(
				-density * u_cloud_extinction * step_len * 2.2);
			vec3 direct = sunset_tint * sun_color * sun_irradiance
				* phase_single * light_trans * planet_vis * sun_air
				* mix(0.58, 1.03, powder);

			float day = smoothstep(-0.16, 0.12, sun_mu);
			vec3 ambient = mix(
				vec3(0.0025, 0.0045, 0.010),
				vec3(0.050, 0.071, 0.102),
				day);
			float ms_energy = (0.010 + 0.032 * (1.0 - light_trans))
				* (0.55 + 0.45 * powder);
			vec3 multiple = sunset_tint * sun_color * sun_irradiance
				* phase_multiple * ms_energy * planet_vis * day;

			radiance += transmittance * sample_alpha
				* (direct + ambient + multiple);
			transmittance *= 1.0 - sample_alpha;
		}
		t += step_len;
	}

	return vec4(radiance, clamp(transmittance, 0.0, 1.0));
}
