shader_type spatial;
render_mode unshaded, blend_premul_alpha, depth_draw_never, depth_test_disabled, cull_disabled;
/// Asterra volumetric clouds.
///
/// Large-scale occupancy comes from the dynamic weather simulation through the
/// shared cloud_structure field. Tiled 3D textures are only used to modulate
/// density *inside* those bodies and to erode their boundaries. This prevents
/// the texture volume from becoming the weather map and repeating over oceans.

#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/cloud_structure.gdshaderinc"
#include "res://shaders/planet_lighting.gdshaderinc"

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
uniform float u_orbit_lod_start = 18000.0;
uniform float u_orbit_shell_altitude = 8200.0;

uniform float u_extinction = 0.00195;
uniform float u_core_extinction_boost = 2.45;
uniform float u_transmittance_cutoff = 0.006;
uniform float u_opaque_core_path = 1100.0;
uniform float u_dense_step = 520.0;
uniform float u_empty_step = 1900.0;
uniform float u_density_scale = 1.10;
uniform float u_wind_speed = 1.0;
uniform float u_detail_strength = 0.24;
uniform float u_lighting_stride = 2100.0;
uniform float u_multiple_scatter_strength = 0.40;
uniform float u_direct_light_strength = 0.95;
uniform float u_ambient_strength = 0.78;

uniform vec4 u_lightning0 = vec4(0.0);
uniform vec4 u_lightning1 = vec4(0.0);
uniform vec4 u_lightning2 = vec4(0.0);
uniform vec4 u_lightning3 = vec4(0.0);

const int MAX_STEPS = 72;
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

vec3 cloud_space(vec3 p) {
	return vec3(
		dot(p, vec3(0.788, 0.326, -0.522)),
		dot(p, vec3(-0.211, 0.943, 0.259)),
		dot(p, vec3(0.578, -0.094, 0.811))
	);
}

float cloud_low_top(vec4 w, float cloud_base) {
	float coverage = clamp(w.r, 0.0, 1.0);
	float type = cloud_type_from_weather(w);
	float stratus_top = cloud_base + mix(900.0, 2100.0, coverage);
	float cumulus_top = cloud_base + mix(2300.0, 5200.0, coverage);
	float cb_top = cloud_base + mix(7600.0, 14800.0, pow(type, 0.72));
	float cu_mix = smoothstep(0.18, 0.52, type);
	float cb_mix = smoothstep(0.56, 0.86, type);
	float top = mix(stratus_top, cumulus_top, cu_mix);
	top = mix(top, cb_top, cb_mix);
	return min(top, 17800.0);
}

float low_vertical_profile(float h01, float type) {
	float stratus = smoothstep(0.00, 0.035, h01) * (1.0 - smoothstep(0.74, 1.0, h01));
	float cumulus = smoothstep(0.00, 0.075, h01) * (1.0 - smoothstep(0.70, 1.0, h01));
	cumulus *= mix(1.0, 0.80 + 0.28 * sin(h01 * PI), 0.65);
	float tower = smoothstep(0.00, 0.055, h01) * (1.0 - smoothstep(0.86, 1.0, h01));
	tower *= 0.86 + 0.28 * smoothstep(0.18, 0.72, h01);
	float a = smoothstep(0.16, 0.52, type);
	float b = smoothstep(0.56, 0.84, type);
	return mix(mix(stratus, cumulus, a), tower, b);
}

float high_vertical_profile(float h01) {
	return smoothstep(0.0, 0.09, h01) * (1.0 - smoothstep(0.66, 1.0, h01));
}

float cloud_density(vec3 p, float detail_weight) {
	float r = length(p);
	float altitude = r - u_planet_radius;
	if (altitude < 500.0 || altitude > 18500.0) return 0.0;

	vec3 d = p / max(r, 1.0);
	vec4 w = weather_at(d);
	if (max(max(w.r, w.g), w.b) < 0.008) return 0.0;
	vec2 structure_noise = cloud_structure_noises(d);
	float low_mask = cloud_low_structure_mask(w, structure_noise);
	float high_mask = cloud_high_structure_mask(w, structure_noise);
	if (max(low_mask, high_mask) < 0.004) return 0.0;

	vec4 flow_data = wind_at(d);
	float coverage = clamp(w.r, 0.0, 1.0);
	float type = cloud_type_from_weather(w);
	float high_cov = clamp(w.b, 0.0, 1.0);
	float cloud_base = clamp(w.a * 6000.0, 700.0, 6500.0);
	float low_top = cloud_low_top(w, cloud_base);

	vec3 east;
	vec3 north;
	tangent_frame(d, east, north);
	vec3 wind_world = (east * flow_data.r + north * flow_data.g) * u_wind_speed;
	vec3 advected = p - wind_world * TIME - d * (TIME * type * 2.2);
	vec3 q = cloud_space(advected);

	float broad_n = texture(u_base_noise, q * 0.0000032).r;
	float middle_n = texture(u_base_noise, q * 0.0000115 + vec3(0.37, 0.11, 0.59)).r;
	float detail_n = 0.50;
	if (detail_weight > 0.04) {
		detail_n = texture(u_detail_noise, q * 0.000052 + vec3(0.17, 0.43, 0.71)).r;
	}

	float vertical_warp = (broad_n - 0.5) * mix(480.0, 760.0, type)
		+ (middle_n - 0.5) * 210.0;
	float shaped_altitude = altitude + vertical_warp;

	float low_density = 0.0;
	if (altitude >= cloud_base - 650.0 && altitude <= low_top + 650.0 && low_mask > 0.002) {
		float h01 = remap01(shaped_altitude, cloud_base, low_top);
		float profile = low_vertical_profile(h01, type);
		float interior = cloud_low_edge_interior(w, structure_noise);
		float shape_noise = broad_n * 0.63 + middle_n * 0.29 + detail_n * (0.08 * detail_weight)
			+ 0.04 * (1.0 - detail_weight);
		float edge_body = smoothstep(0.34, 0.66, shape_noise);
		float interior_density = mix(0.72, 1.13, broad_n) * mix(0.88, 1.08, middle_n);
		float erosion_mix = clamp(interior * (1.0 - type * 0.30), 0.0, 1.0);
		float body = mix(edge_body, interior_density, erosion_mix) * low_mask;
		if (detail_weight > 0.04) {
			float edge_weight = 1.0 - interior;
			body -= (1.0 - detail_n) * u_detail_strength * detail_weight * edge_weight;
		}
		body = max(body, 0.0);
		float water_loading = 0.68 + coverage * 0.24 + type * 0.58 + flow_data.b * 0.20;
		low_density = body * profile * water_loading;
	}

	float high_density = 0.0;
	float high_start = mix(9000.0, 7600.0, type);
	if (altitude >= high_start - 500.0 && altitude <= 17800.0 && high_mask > 0.002) {
		float hhi = remap01(shaped_altitude, high_start, 17400.0);
		float profile_hi = high_vertical_profile(hhi);
		float wispy = texture(u_base_noise,
			q * vec3(0.0000016, 0.0000026, 0.0000016) + vec3(0.31, 0.07, 0.53)).r;
		float interior_hi = cloud_high_edge_interior(w, structure_noise);
		float filament = mix(smoothstep(0.30, 0.70, wispy), mix(0.70, 1.08, wispy), interior_hi);
		high_density = high_mask * filament * profile_hi * mix(0.12, 0.52, high_cov) * (0.72 + type * 0.34);
	}

	return clamp((low_density + high_density) * u_density_scale, 0.0, 1.65);
}

float cloud_density_light(vec3 p) {
	float r = length(p);
	float altitude = r - u_planet_radius;
	if (altitude < 500.0 || altitude > 18500.0) return 0.0;
	vec3 d = p / max(r, 1.0);
	vec4 w = weather_at(d);
	if (max(max(w.r, w.g), w.b) < 0.008) return 0.0;
	vec2 n = cloud_structure_noises(d);
	float low_mask = cloud_low_structure_mask(w, n);
	float high_mask = cloud_high_structure_mask(w, n);
	float type = cloud_type_from_weather(w);
	float coverage = clamp(w.r, 0.0, 1.0);
	float cloud_base = clamp(w.a * 6000.0, 700.0, 6500.0);
	float low_top = cloud_low_top(w, cloud_base);

	float low = 0.0;
	if (altitude >= cloud_base && altitude <= low_top) {
		float h01 = remap01(altitude, cloud_base, low_top);
		low = low_mask * low_vertical_profile(h01, type) * (0.58 + coverage * 0.30 + type * 0.46);
	}
	float high = 0.0;
	float high_start = mix(9000.0, 7600.0, type);
	if (altitude >= high_start && altitude <= 17400.0) {
		float hhi = remap01(altitude, high_start, 17400.0);
		high = high_mask * high_vertical_profile(hhi) * mix(0.10, 0.42, clamp(w.b, 0.0, 1.0));
	}
	return clamp((low + high) * u_density_scale, 0.0, 1.45);
}

float optical_density(float density) {
	float core = smoothstep(0.10, 0.72, density);
	return density * mix(1.0, u_core_extinction_boost, core);
}

float cloud_sun_optical_depth(vec3 p, vec3 sun_dir) {
	vec2 shell = sphere_hit(p, sun_dir, u_cloud_outer_radius);
	float max_dist = max(shell.y, 0.0);
	if (max_dist <= 1.0) return 0.0;
	float step_len = max_dist / float(SUN_SHADOW_STEPS);
	float tau = 0.0;
	for (int i = 0; i < SUN_SHADOW_STEPS; i++) {
		float t = (float(i) + 0.5) * step_len;
		float d = optical_density(cloud_density_light(p + sun_dir * t));
		tau += d * step_len * u_extinction * 0.88;
		if (tau > 8.0) break;
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

vec3 cloud_multiple_scattering(float sun_tau, float density, float mu, float planet_vis) {
	float order2 = exp(-sun_tau * 0.35);
	float order3 = exp(-sun_tau * 0.12);
	float body = 1.0 - exp(-density * 2.4);
	float angle = 0.72 + 0.28 * clamp(mu * 0.5 + 0.5, 0.0, 1.0);
	float energy = (order2 * 0.16 + order3 * 0.048) * u_multiple_scatter_strength;
	energy *= body * angle * planet_vis;
	return mix(vec3(0.50, 0.60, 0.76), vec3(1.0, 0.91, 0.78), smoothstep(-0.12, 0.32, mu)) * energy;
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
	vec3 probe = cam + delta * 0.5;
	float mid_height = max(length(probe) - u_planet_radius, 0.0);
	float air = exp(-mid_height / 8000.0);
	float amount = clamp(1.0 - exp(-seg * air * 1.25e-5), 0.0, 0.82);
	float planet_vis = planet_direct_sun_visibility(probe, sun_dir, u_planet_radius);
	if (planet_vis <= 0.001) return vec4(0.0);
	float mu = dot(normalize(delta), sun_dir);
	vec3 tint = mix(vec3(0.18, 0.31, 0.55), vec3(0.78, 0.69, 0.57), smoothstep(-0.12, 0.35, mu));
	float horizon = 1.0 - abs(dot(normalize(probe), normalize(delta)));
	tint *= (0.38 + horizon * 0.34) * planet_vis;
	return vec4(tint, amount * 0.50 * planet_vis);
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
	if (max(max(w.r, w.g), w.b) < 0.008) return;
	vec2 n = cloud_structure_noises(d);
	float low_mask = cloud_low_structure_mask(w, n);
	float high_mask = cloud_high_structure_mask(w, n);
	float low_interior = cloud_low_edge_interior(w, n);
	float high_interior = cloud_high_edge_interior(w, n);
	float fine = cloud_struct_vnoise(d * 118.0 + vec3(31.7, -16.2, 8.4));
	float low_edge_detail = mix(smoothstep(0.30, 0.68, fine), 1.0, low_interior);
	float high_edge_detail = mix(smoothstep(0.24, 0.72, 1.0 - fine), 1.0, high_interior);
	float low_alpha = low_mask * low_edge_detail * mix(0.38, 0.94, clamp(w.r, 0.0, 1.0));
	float high_alpha = high_mask * high_edge_detail * mix(0.10, 0.48, clamp(w.b, 0.0, 1.0));
	alpha = clamp(max(low_alpha, high_alpha), 0.0, 0.96);
	if (alpha < 0.003) {
		alpha = 0.0;
		return;
	}

	float planet_vis = planet_direct_sun_visibility(p, sun_dir, u_planet_radius);
	vec3 skylight = planet_sky_irradiance(p, sun_dir, u_planet_radius) * u_ambient_strength;
	float type = cloud_type_from_weather(w);
	float dense = clamp(low_alpha * (0.65 + type * 0.45) + high_alpha * 0.28, 0.0, 1.0);
	float front = pow(max(dot(-rd, sun_dir), 0.0), 9.0);
	vec3 direct = vec3(1.0, 0.91, 0.79) * planet_vis * (0.18 + 0.52 * max(dot(d, sun_dir), 0.0));
	direct *= mix(1.0, 0.50, dense);
	direct += vec3(1.0, 0.84, 0.68) * front * planet_vis * 0.22;
	color = vec3(0.72, 0.75, 0.79) * (skylight + direct);
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
	bool use_orbit_lod = camera_alt >= u_orbit_lod_start;

	if (use_orbit_lod) {
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
		if (possible < 0.008) discard;

		float base_dense_step = clamp(u_dense_step, 360.0, 820.0);
		float base_empty_step = max(clamp(u_empty_step, base_dense_step * 2.0, 2800.0), base_dense_step);
		float lighting_stride = max(u_lighting_stride, base_dense_step * 2.6);
		float jitter = screen_jitter(FRAGCOORD.xy);
		float t = t0 + base_dense_step * (0.08 + 0.84 * jitter);
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
			float distance_lod = smoothstep(24000.0, 150000.0, t);
			float step_scale = mix(1.0, 2.7, distance_lod);
			float dense_step = min(base_dense_step * step_scale, 1800.0);
			float empty_step = min(base_empty_step * step_scale, 6500.0);
			float detail_weight = 1.0 - smoothstep(18000.0, 72000.0, t);
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
				t += min(dense_step, skip_len * 0.36);
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
			float sun_tau = cached_sun_tau + extinction_density * u_extinction * min(step_len, 620.0) * 0.20;
			float sun_trans = exp(-sun_tau);
			float planet_vis = planet_direct_sun_visibility(sample_pos, sun_dir, u_planet_radius);
			vec3 ambient = planet_sky_irradiance(sample_pos, sun_dir, u_planet_radius) * u_ambient_strength;
			float core = smoothstep(0.12, 0.78, density);
			float sky_access = mix(0.16, 0.92, pow(max(sun_trans, 0.0), 0.24));
			sky_access *= mix(1.0, 0.48, core * (1.0 - sqrt(max(sun_trans, 0.0))));
			float powder = 1.0 - exp(-density * 3.2);
			vec3 direct = vec3(1.0, 0.91, 0.79) * planet_vis * sun_trans
				* (0.035 + phase * 0.64) * (0.54 + powder * 0.46) * u_direct_light_strength;
			vec3 multiple = cloud_multiple_scattering(sun_tau, density, mu, planet_vis);
			vec3 sample_light = ambient * sky_access + direct + multiple;

			float electric = lightning_illumination(sample_pos);
			if (electric > 0.001) {
				sample_light += vec3(0.42, 0.62, 1.0) * electric * (0.20 + powder * 0.46);
			}

			float contribution = transmittance * alpha_step;
			radiance += contribution * sample_light;
			optical_t_sum += contribution * (t + step_len * 0.5);
			optical_weight += contribution;
			transmittance *= step_transmittance;
			float core_presence = smoothstep(0.18, 0.58, density);
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
