shader_type spatial;
render_mode unshaded, blend_premul_alpha, depth_draw_never, depth_test_disabled, cull_disabled;
/// Asterra volumetric clouds.
///
/// Below the upper atmosphere this is a true 3D density raymarch. From aircraft
/// altitude upward the geometric thickness of individual march steps becomes
/// visible and expensive, so a dedicated cloud shell LOD replaces the raymarch
/// with a handful of weather/noise samples.

#include "res://shaders/planet_surface_lookup.gdshaderinc"

uniform sampler2D u_scene_depth : hint_depth_texture, repeat_disable, filter_nearest;
uniform sampler2DArray u_weather_map : filter_linear, repeat_disable;
uniform sampler2DArray u_wind_map : filter_linear, repeat_disable;
uniform sampler3D u_base_noise : filter_linear, repeat_enable;
uniform sampler3D u_detail_noise : filter_linear, repeat_enable;

uniform float u_weather_face_res = 192.0;
uniform vec3 u_camera_world = vec3(1000000.0, 0.0, 0.0);
uniform vec3 u_sun_dir = vec3(1.0, 0.15, 0.3);
uniform float u_planet_radius = 1000000.0;
uniform float u_atmosphere_radius = 1060000.0;
uniform float u_cloud_outer_radius = 1018500.0;
uniform float u_orbit_lod_start = 22000.0;
uniform float u_orbit_shell_altitude = 8200.0;

uniform float u_extinction = 0.00215;
uniform float u_core_extinction_boost = 3.15;
uniform float u_transmittance_cutoff = 0.004;
uniform float u_opaque_core_path = 760.0;
uniform float u_dense_step = 460.0;
uniform float u_empty_step = 1700.0;
uniform float u_density_scale = 1.18;
uniform float u_wind_speed = 1.0;
uniform float u_detail_strength = 0.27;
uniform float u_sun_shadow_dense_step = 440.0;
uniform float u_sun_shadow_empty_step = 900.0;
uniform float u_lighting_stride = 1800.0;
uniform float u_multiple_scatter_strength = 0.42;
uniform float u_direct_light_strength = 0.92;
uniform float u_ambient_strength = 0.72;

uniform vec4 u_lightning0 = vec4(0.0);
uniform vec4 u_lightning1 = vec4(0.0);
uniform vec4 u_lightning2 = vec4(0.0);
uniform vec4 u_lightning3 = vec4(0.0);

const int MAX_STEPS = 96;
const int SUN_SHADOW_STEPS = 4;
const float BIG = 1.0e20;

void vertex() {
	POSITION = vec4(VERTEX.xy, 1.0, 1.0);
}

vec2 sphere_hit(vec3 ro, vec3 rd, float radius) {
	float b = dot(ro, rd);
	float c = dot(ro, ro) - radius * radius;
	float h = b * b - c;
	if (h < 0.0) return vec2(BIG, -BIG);
	float s = sqrt(h);
	return vec2(-b - s, -b + s);
}

vec4 weather_at(vec3 dir) {
	return texture(u_weather_map, surface_array_coord(normalize(dir), u_weather_face_res));
}

vec4 wind_at(vec3 dir) {
	return texture(u_wind_map, surface_array_coord(normalize(dir), u_weather_face_res));
}

void tangent_frame(vec3 d, out vec3 east, out vec3 north) {
	vec3 ref = abs(d.y) < 0.985 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	east = normalize(cross(ref, d));
	north = normalize(cross(d, east));
}

float remap01(float v, float lo, float hi) {
	return clamp((v - lo) / max(hi - lo, 1e-5), 0.0, 1.0);
}

float vertical_low_profile(float h01, float convective) {
	float base_width = mix(0.020, 0.006, clamp(convective, 0.0, 1.0));
	float base = smoothstep(0.0, base_width, h01);
	float cap = 1.0 - smoothstep(mix(0.70, 0.91, convective), 1.0, h01);
	float deck = 1.0 - h01 * 0.10;
	float tower = smoothstep(0.010, 0.16, h01) * (1.0 - 0.06 * h01);
	return base * cap * mix(deck, tower, convective);
}

vec3 cloud_space(vec3 p) {
	return vec3(
		dot(p, vec3(0.788, 0.326, -0.522)),
		dot(p, vec3(-0.211, 0.943, 0.259)),
		dot(p, vec3(0.578, -0.094, 0.811))
	);
}

float cloud_density(vec3 p, float detail_weight) {
	float r = length(p);
	float altitude = r - u_planet_radius;
	if (altitude < 500.0 || altitude > 18500.0) return 0.0;

	vec3 d = p / max(r, 1.0);
	vec4 w = weather_at(d);
	vec4 flow_data = wind_at(d);
	float coverage = w.r;
	float convective = w.g;
	float high_cov = w.b;
	if (max(max(coverage, convective), high_cov) < 0.010) return 0.0;

	float cloud_base = clamp(w.a * 6000.0, 700.0, 6500.0);
	float low_top_flat = cloud_base + mix(1100.0, 3300.0, coverage);
	float low_top_tower = cloud_base + mix(3800.0, 14500.0, pow(convective, 0.72));
	float tower_mix = smoothstep(0.10, 0.64, convective);
	float low_top = min(mix(low_top_flat, low_top_tower, tower_mix), 17800.0);

	vec3 east;
	vec3 north;
	tangent_frame(d, east, north);
	vec3 wind_world = (east * flow_data.r + north * flow_data.g) * u_wind_speed;
	vec3 advected = p - wind_world * TIME - d * (TIME * convective * 3.1);
	vec3 q = cloud_space(advected);
	float broad_n = texture(u_base_noise, q * 0.0000032).r;
	float middle_n = texture(u_base_noise, q * 0.0000115 + vec3(0.37, 0.11, 0.59)).r;
	float detail_n = 0.50;
	if (detail_weight > 0.015) detail_n = texture(u_detail_noise, q * 0.000052 + vec3(0.17, 0.43, 0.71)).r;

	float low_density = 0.0;
	if (altitude >= cloud_base && altitude <= low_top) {
		float h01 = remap01(altitude, cloud_base, low_top);
		float profile = vertical_low_profile(h01, convective);
		float overcast = smoothstep(0.68, 0.96, coverage);
		float threshold = mix(0.73, 0.36, coverage) - convective * (0.08 + 0.11 * h01);
		float tower_body = 0.12 * convective * smoothstep(0.20, 0.78, h01);
		float shape = broad_n * 0.58 + middle_n * 0.29 + detail_n * (0.13 * detail_weight)
			+ 0.065 * (1.0 - detail_weight) + overcast * 0.12 + tower_body;
		float body = smoothstep(threshold, min(threshold + 0.17, 0.98), shape);
		if (detail_weight > 0.015) {
			float edge_weight = 1.0 - body;
			float erosion = (1.0 - detail_n) * u_detail_strength * detail_weight * edge_weight * (1.0 - convective * 0.68);
			body = max(body - erosion, 0.0);
		}
		low_density = body * profile * (0.70 + convective * 0.72 + flow_data.b * 0.26);
	}

	float high_start = mix(8200.0, 9800.0, 1.0 - high_cov);
	float high_density = 0.0;
	if (altitude >= high_start && altitude <= 17000.0 && high_cov > 0.01) {
		float hhi = remap01(altitude, high_start, 17000.0);
		float profile_hi = smoothstep(0.0, 0.10, hhi) * (1.0 - smoothstep(0.72, 1.0, hhi));
		float wispy = texture(u_base_noise, q * vec3(0.0000017, 0.0000027, 0.0000017) + vec3(0.31, 0.07, 0.53)).r;
		float filaments = 0.50;
		if (detail_weight > 0.08) filaments = texture(u_detail_noise, q * 0.000020 + vec3(0.61, 0.13, 0.29)).r;
		float hi_threshold = mix(0.84, 0.39, high_cov);
		float hi = smoothstep(hi_threshold, min(hi_threshold + 0.17, 0.98),
			wispy * 0.82 + filaments * (0.18 * detail_weight) + 0.09 * (1.0 - detail_weight) + convective * 0.06);
		high_density = hi * profile_hi * mix(0.18, 0.55, high_cov);
	}
	return clamp((low_density + high_density) * u_density_scale, 0.0, 1.7);
}

float cloud_density_light(vec3 p) {
	float r = length(p);
	float altitude = r - u_planet_radius;
	if (altitude < 500.0 || altitude > 18500.0) return 0.0;
	vec3 d = p / max(r, 1.0);
	vec4 w = weather_at(d);
	float coverage = w.r;
	float convective = w.g;
	float high_cov = w.b;
	if (max(max(coverage, convective), high_cov) < 0.010) return 0.0;
	float cloud_base = clamp(w.a * 6000.0, 700.0, 6500.0);
	float low_top_flat = cloud_base + mix(1100.0, 3300.0, coverage);
	float low_top_tower = cloud_base + mix(3800.0, 14500.0, pow(convective, 0.72));
	float low_top = min(mix(low_top_flat, low_top_tower, smoothstep(0.10, 0.64, convective)), 17800.0);
	vec3 q = cloud_space(p) + vec3(TIME * 0.000003, 0.0, TIME * -0.000002);
	float broad = texture(u_base_noise, q * 0.0000032).r;
	float low = 0.0;
	if (altitude >= cloud_base && altitude <= low_top) {
		float h01 = remap01(altitude, cloud_base, low_top);
		float profile = vertical_low_profile(h01, convective);
		float threshold = mix(0.72, 0.34, coverage) - convective * 0.09;
		float body = smoothstep(threshold, min(threshold + 0.22, 0.98), broad + smoothstep(0.68, 0.96, coverage) * 0.10 + convective * 0.08);
		low = body * profile * (0.72 + convective * 0.70);
	}
	float high = 0.0;
	if (altitude >= 8500.0 && altitude <= 17000.0 && high_cov > 0.01) {
		float hhi = remap01(altitude, 8500.0, 17000.0);
		float profile = smoothstep(0.0, 0.12, hhi) * (1.0 - smoothstep(0.70, 1.0, hhi));
		float body = smoothstep(mix(0.86, 0.42, high_cov), 0.96, broad + convective * 0.05);
		high = body * profile * mix(0.16, 0.48, high_cov);
	}
	return clamp((low + high) * u_density_scale, 0.0, 1.6);
}

float optical_density(float density) {
	float core = smoothstep(0.08, 0.58, density);
	return density * mix(1.0, u_core_extinction_boost, core);
}

float hash13(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.yzx + 33.33);
	return fract((p.x + p.y) * p.z);
}

float cloud_sun_optical_depth(vec3 p, vec3 sun_dir) {
	vec2 shell = sphere_hit(p, sun_dir, u_cloud_outer_radius);
	float max_dist = max(shell.y, 0.0);
	if (max_dist <= 1.0) return 0.0;
	vec3 ref = abs(sun_dir.y) < 0.97 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 side0 = normalize(cross(ref, sun_dir));
	vec3 side1 = normalize(cross(sun_dir, side0));
	float seed = hash13(p * 0.00073 + sun_dir * 31.7);
	float travel = clamp(u_sun_shadow_dense_step, 300.0, 700.0) * mix(0.84, 1.16, seed);
	float growth = clamp(u_sun_shadow_empty_step / max(u_sun_shadow_dense_step, 1.0), 1.75, 2.15);
	float previous = 0.0;
	float tau = 0.0;
	for (int i = 0; i < SUN_SHADOW_STEPS; i++) {
		if (travel >= max_dist || tau >= 8.0) break;
		float segment_len = max(travel - previous, 160.0);
		float fi = float(i);
		float angle = seed * 6.2831853 + fi * 2.3999632;
		float cone_radius = min(70.0 + travel * 0.085, 900.0);
		vec3 cone_offset = (side0 * cos(angle) + side1 * sin(angle)) * cone_radius;
		float d = optical_density(cloud_density_light(p + sun_dir * travel + cone_offset));
		tau += d * segment_len * u_extinction * 1.12;
		previous = travel;
		travel *= growth;
	}
	return tau;
}

float hg(float g, float mu) {
	float gg = g * g;
	return (1.0 - gg) / (4.0 * PI * pow(max(1.0 + gg - 2.0 * g * mu, 1e-4), 1.5));
}

float screen_jitter(vec2 p) {
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float planet_sun_visibility(vec3 p, vec3 sun_dir) {
	vec2 hit = sphere_hit(p + sun_dir * 4.0, sun_dir, u_planet_radius + 20.0);
	if (hit.x > 0.0 && hit.y > 0.0) return 0.0;
	return 1.0;
}

vec3 cloud_multiple_scattering(float sun_tau, float density, float mu, float sun_visible, float twilight) {
	float order2 = exp(-sun_tau * 0.34);
	float order3 = exp(-sun_tau * 0.115);
	float body = 1.0 - exp(-density * 2.6);
	float angle = 0.72 + 0.28 * clamp(mu * 0.5 + 0.5, 0.0, 1.0);
	float energy = (order2 * 0.17 + order3 * 0.050) * u_multiple_scatter_strength;
	energy *= body * angle * sun_visible * twilight;
	return mix(vec3(0.54, 0.64, 0.78), vec3(1.0, 0.91, 0.78), smoothstep(-0.12, 0.32, mu)) * energy;
}

float lightning_one(vec3 p, vec4 flash) {
	if (flash.w <= 0.001) return 0.0;
	float radius = mix(4200.0, 26000.0, clamp(flash.w / 12.0, 0.0, 1.0));
	float x = length(p - flash.xyz) / max(radius, 1.0);
	return flash.w * exp(-x * x * 2.15);
}

float lightning_illumination(vec3 p) {
	return lightning_one(p, u_lightning0) + lightning_one(p, u_lightning1)
		+ lightning_one(p, u_lightning2) + lightning_one(p, u_lightning3);
}

vec4 cloud_aerial_fog(vec3 cam, vec3 frag, vec3 sun_dir) {
	vec3 delta = frag - cam;
	float seg = length(delta);
	if (seg < 250.0) return vec4(0.0);
	vec3 mid = normalize(cam + delta * 0.5);
	float mid_height = max(length(cam + delta * 0.5) - u_planet_radius, 0.0);
	float air = exp(-mid_height / 8000.0);
	float amount = clamp(1.0 - exp(-seg * air * 1.25e-5), 0.0, 0.82);
	float mu = dot(normalize(delta), sun_dir);
	vec3 tint = mix(vec3(0.28, 0.43, 0.66), vec3(0.78, 0.70, 0.58), smoothstep(-0.12, 0.35, mu));
	float horizon = 1.0 - abs(dot(mid, normalize(delta)));
	tint *= 0.42 + horizon * 0.34;
	return vec4(tint, amount * 0.52);
}

void orbit_cloud(vec3 ro, vec3 rd, float scene_dist, vec3 sun_dir, out vec3 color, out float alpha) {
	color = vec3(0.0);
	alpha = 0.0;
	float shell_radius = u_planet_radius + u_orbit_shell_altitude;
	vec2 hit = sphere_hit(ro, rd, shell_radius);
	if (hit.x > hit.y || hit.y <= 0.0) return;
	float t = hit.x > 0.0 ? hit.x : hit.y;
	if (t <= 0.0) return;
	vec2 ground = sphere_hit(ro, rd, u_planet_radius);
	if (ground.x > 0.0 && t >= ground.x) return;
	if (scene_dist < BIG * 0.5 && t >= scene_dist - 4.0) return;

	vec3 p = ro + rd * t;
	vec3 d = normalize(p);
	vec4 w = weather_at(d);
	float coverage = clamp(max(w.r, w.b * 0.72) + w.g * 0.12, 0.0, 1.0);
	if (coverage < 0.012) return;

	vec3 q = cloud_space(p + vec3(TIME * 13.0, 0.0, TIME * -8.0));
	float broad = texture(u_base_noise, q * 0.0000032).r;
	float middle = texture(u_base_noise, q * 0.000012 + vec3(0.37, 0.11, 0.59)).r;
	float overcast = smoothstep(0.68, 0.95, w.r);
	float shape = broad * 0.68 + middle * 0.32 + w.g * 0.10 + overcast * 0.16;
	float threshold = mix(0.78, 0.37, coverage);
	float body = smoothstep(threshold, min(threshold + 0.14, 0.98), shape);
	float high_veil = smoothstep(0.22, 0.85, w.b) * smoothstep(0.42, 0.72, broad) * 0.48;
	alpha = clamp(max(body, high_veil) * mix(0.72, 0.97, coverage), 0.0, 0.96);
	if (alpha < 0.004) {
		alpha = 0.0;
		return;
	}

	float sun_elev = dot(d, sun_dir);
	float twilight = smoothstep(-0.20, 0.08, sun_elev);
	float front = pow(max(dot(-rd, sun_dir), 0.0), 10.0);
	float dense = smoothstep(0.38, 0.92, body);
	vec3 night = vec3(0.012, 0.018, 0.032);
	vec3 day = vec3(0.73, 0.76, 0.79) * (0.40 + 0.60 * sqrt(max(sun_elev, 0.0)));
	day *= mix(1.0, 0.68, dense);
	day += vec3(1.0, 0.88, 0.72) * front * 0.34;
	color = mix(night, day, twilight);

	vec4 aerial = cloud_aerial_fog(ro, p, sun_dir);
	color = mix(color, aerial.rgb, aerial.a);
}

void fragment() {
	vec2 screen_uv = FRAGCOORD.xy / VIEWPORT_SIZE;
	float raw_depth = texture(u_scene_depth, screen_uv).r;
	float scene_dist = BIG;
	if (raw_depth > 1e-6) {
		vec3 depth_ndc = vec3(screen_uv * 2.0 - 1.0, raw_depth);
		vec4 depth_view = INV_PROJECTION_MATRIX * vec4(depth_ndc, 1.0);
		depth_view.xyz /= max(abs(depth_view.w), 1e-8);
		scene_dist = length(depth_view.xyz);
	}
	vec4 view_far = INV_PROJECTION_MATRIX * vec4(screen_uv * 2.0 - 1.0, 0.0, 1.0);
	vec3 view_dir = normalize(view_far.xyz / max(abs(view_far.w), 1e-8));
	vec3 rd = normalize((INV_VIEW_MATRIX * vec4(view_dir, 0.0)).xyz);
	vec3 ro = u_camera_world;
	vec3 sun_dir = normalize(u_sun_dir);
	float camera_alt = length(ro) - u_planet_radius;

	if (camera_alt >= u_orbit_lod_start) {
		vec3 orbit_color;
		float orbit_alpha;
		orbit_cloud(ro, rd, scene_dist, sun_dir, orbit_color, orbit_alpha);
		if (orbit_alpha < 0.002) discard;
		ALBEDO = orbit_color * orbit_alpha;
		ALPHA = orbit_alpha;
	} else {
		vec2 outer = sphere_hit(ro, rd, u_cloud_outer_radius);
		if (outer.x > outer.y || outer.y <= 0.0) discard;
		float t0 = max(outer.x, 0.0);
		float t1 = outer.y;
		vec2 ground = sphere_hit(ro, rd, u_planet_radius);
		if (ground.x > 0.0) t1 = min(t1, ground.x);
		if (scene_dist < BIG * 0.5) {
			float depth_guard = max(3.0, scene_dist * 2.0e-6);
			t1 = min(t1, max(scene_dist - depth_guard, 0.0));
		}
		if (t1 <= t0 + 1.0) discard;

		float seg = t1 - t0;
		vec4 w0 = weather_at(normalize(ro + rd * (t0 + seg * 0.20)));
		vec4 w1 = weather_at(normalize(ro + rd * (t0 + seg * 0.50)));
		vec4 w2 = weather_at(normalize(ro + rd * (t0 + seg * 0.80)));
		float possible = max(max(max(w0.r, w0.g), w0.b), max(max(w1.r, w1.g), max(w1.b, max(max(w2.r, w2.g), w2.b))));
		if (possible < 0.010) discard;

		float base_dense_step = clamp(u_dense_step, 260.0, 760.0);
		float base_empty_step = max(clamp(u_empty_step, base_dense_step * 2.0, 2600.0), base_dense_step);
		float lighting_stride = max(u_lighting_stride, base_dense_step * 2.5);
		float jitter = screen_jitter(FRAGCOORD.xy);
		float t = t0 + base_dense_step * (0.10 + 0.20 * jitter);
		float transmittance = 1.0;
		vec3 radiance = vec3(0.0);
		float optical_t_sum = 0.0;
		float optical_weight = 0.0;
		float dense_path = 0.0;
		float mu = dot(rd, sun_dir);
		float phase = hg(0.72, mu) * 0.78 + hg(-0.20, mu) * 0.22;
		phase = min(phase * 4.0 * PI, 4.25);
		float cached_sun_tau = 0.0;
		float cached_sun_t = -BIG;

		for (int i = 0; i < MAX_STEPS; i++) {
			if (t >= t1 || transmittance <= 0.0) break;
			float distance_lod = smoothstep(26000.0, 180000.0, t);
			float step_scale = mix(1.0, 3.0, distance_lod);
			float dense_step = min(base_dense_step * step_scale, 1700.0);
			float empty_step = min(base_empty_step * step_scale, 6200.0);
			float detail_weight = 1.0 - smoothstep(22000.0, 90000.0, t);
			vec3 p = ro + rd * t;
			float density = cloud_density(p, detail_weight);
			if (density <= 0.0015) {
				float skip_len = min(empty_step, t1 - t);
				float probe_t = t + skip_len * 0.5;
				if (cloud_density_light(ro + rd * probe_t) <= 0.0015) {
					if (skip_len >= lighting_stride * 0.70) cached_sun_t = -BIG;
					t += skip_len;
					continue;
				}
				t += min(dense_step, skip_len * 0.34);
				continue;
			}

			float step_len = min(dense_step, t1 - t);
			float extinction_density = optical_density(density);
			float optical = extinction_density * step_len * u_extinction;
			float step_transmittance = exp(-optical);
			float alpha_step = 1.0 - step_transmittance;
			vec3 sample_pos = p + rd * (step_len * 0.5);
			if (cached_sun_t < -BIG * 0.5 || (t - cached_sun_t) >= lighting_stride * step_scale) {
				cached_sun_tau = cloud_sun_optical_depth(sample_pos, sun_dir);
				cached_sun_t = t;
			}
			float sun_tau = cached_sun_tau + extinction_density * u_extinction * min(step_len, 520.0) * 0.24;
			float sun_trans = exp(-sun_tau);
			float sun_visible = planet_sun_visibility(sample_pos, sun_dir);
			vec3 up = normalize(sample_pos);
			float sun_elev = dot(up, sun_dir);
			float twilight = smoothstep(-0.22, 0.10, sun_elev);
			vec3 ambient = mix(vec3(0.00045, 0.00070, 0.00145), vec3(0.135, 0.180, 0.245) * u_ambient_strength, twilight);
			float core = smoothstep(0.10, 0.75, density);
			float sky_access = mix(0.08, 0.86, pow(max(sun_trans, 0.0), 0.28));
			sky_access *= mix(1.0, 0.42, core * (1.0 - sqrt(max(sun_trans, 0.0))));
			float powder = 1.0 - exp(-density * 3.4);
			vec3 direct = vec3(1.0, 0.92, 0.80) * sun_visible * sun_trans
				* (0.045 + phase * 0.62) * (0.56 + powder * 0.48) * u_direct_light_strength;
			vec3 multiple = cloud_multiple_scattering(sun_tau, density, mu, sun_visible, twilight);
			vec3 sample_light = ambient * sky_access + direct + multiple;
			float electric = lightning_illumination(sample_pos);
			if (electric > 0.001) sample_light += vec3(0.42, 0.62, 1.0) * electric * (0.20 + powder * 0.46);
			float contribution = transmittance * alpha_step;
			radiance += contribution * sample_light;
			optical_t_sum += contribution * (t + step_len * 0.5);
			optical_weight += contribution;
			transmittance *= step_transmittance;
			float core_presence = smoothstep(0.16, 0.52, density);
			dense_path += step_len * core_presence;
			if (dense_path >= u_opaque_core_path) {
				radiance += transmittance * sample_light;
				transmittance = 0.0;
				break;
			}
			if (transmittance < u_transmittance_cutoff) {
				transmittance = 0.0;
				break;
			}
			t += step_len;
		}

		float alpha = 1.0 - transmittance;
		if (alpha < 0.002) discard;
		vec3 cloud_color = radiance / max(alpha, 1e-4);
		if (optical_weight > 1e-5) {
			float representative_t = optical_t_sum / optical_weight;
			vec4 aerial = cloud_aerial_fog(ro, ro + rd * representative_t, sun_dir);
			cloud_color = mix(cloud_color, aerial.rgb, aerial.a);
		}
		ALBEDO = cloud_color * alpha;
		ALPHA = alpha;
	}
}
