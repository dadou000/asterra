shader_type spatial;
render_mode unshaded, blend_premul_alpha, depth_draw_never, depth_test_disabled, cull_disabled;
/// Asterra volumetric clouds -- single volumetric path.
///
/// During this correctness pass there is deliberately NO orbit shell/2D cloud
/// representation. Ground, aircraft and orbit cameras all raymarch the same
/// spherical cloud volume so layer hand-offs cannot masquerade as cloud bugs.
///
/// Weather owns macro geography and cloud type. The shared cloud_structure
/// include owns base/top/profile rules used by both this renderer and the
/// surface shadow integrator. 3D textures only create local shape and erosion.

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

uniform float u_extinction = 0.00155;
uniform float u_core_extinction_boost = 2.05;
uniform float u_transmittance_cutoff = 0.004;
uniform float u_dense_step = 280.0;
uniform float u_empty_step = 1050.0;
uniform float u_density_scale = 1.04;
uniform float u_wind_speed = 1.0;
uniform float u_detail_strength = 0.28;
uniform float u_lighting_stride = 1650.0;
uniform float u_multiple_scatter_strength = 0.38;
uniform float u_direct_light_strength = 0.96;
uniform float u_ambient_strength = 0.72;
uniform float u_sun_shadow_max_distance = 42000.0;

uniform vec4 u_lightning0 = vec4(0.0);
uniform vec4 u_lightning1 = vec4(0.0);
uniform vec4 u_lightning2 = vec4(0.0);
uniform vec4 u_lightning3 = vec4(0.0);

const int MAX_STEPS = 112;
const int SUN_SHADOW_STEPS = 7;
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

vec3 cloud_space(vec3 p) {
	return vec3(
		dot(p, vec3(0.788, 0.326, -0.522)),
		dot(p, vec3(-0.211, 0.943, 0.259)),
		dot(p, vec3(0.578, -0.094, 0.811))
	);
}

/// Full visible density. Macro occupancy and altitude ranges come from the
/// shared model; local base/top warps are independent so a layer cannot become
/// a perfectly concentric spherical slab.
float cloud_density(vec3 p, float detail_weight) {
	float r = length(p);
	float altitude = r - u_planet_radius;
	if (altitude < 450.0 || altitude > 18500.0) return 0.0;

	vec3 d = p / max(r, 1.0);
	vec4 w = weather_at(d);
	if (max(max(w.r, w.g), w.b) < 0.006) return 0.0;

	vec2 low_n = cloud_structure_noises(d);
	vec2 high_n = cloud_high_structure_noises(d);
	float low_mask = cloud_low_structure_mask(w, low_n);
	float high_mask = cloud_high_structure_mask(w, high_n);
	if (max(low_mask, high_mask) < 0.003) return 0.0;

	float type = cloud_type_from_weather(w);
	float coverage = clamp(w.r, 0.0, 1.0);
	float high_cov = clamp(w.b, 0.0, 1.0);
	vec4 flow = wind_at(d);

	vec3 east;
	vec3 north;
	tangent_frame(d, east, north);
	vec3 wind_world = (east * flow.r + north * flow.g) * u_wind_speed;
	// Horizontal weather advection controls macro geography; this sub-grid drift
	// only moves erosion/noise within an existing weather body.
	vec3 advected = p - wind_world * TIME - d * (TIME * type * 1.4);
	vec3 q = cloud_space(advected);

	float broad = texture(u_base_noise, q * 0.0000055 + vec3(0.17, 0.31, 0.07)).r;
	float middle = texture(u_base_noise, q * 0.000022 + vec3(0.37, 0.11, 0.59)).r;
	float top_noise = texture(u_base_noise, q * 0.000009 + vec3(0.71, 0.23, 0.43)).r;
	float detail = 0.50;
	float micro = 0.50;
	if (detail_weight > 0.025) {
		detail = texture(u_detail_noise, q * 0.000095 + vec3(0.17, 0.43, 0.71)).r;
	}
	if (detail_weight > 0.32) {
		micro = texture(u_detail_noise, q * 0.000260 + vec3(0.63, 0.19, 0.37)).r;
	}

	float low_density = 0.0;
	float low_base = cloud_base_from_weather(w);
	float low_top = cloud_low_top_from_weather(w);

	// Cloud bases vary less than tops. Deep convection gets much larger top
	// displacement, which creates towers rather than a thick uniform shell.
	float base_warp = (broad - 0.5) * mix(180.0, 340.0, type)
		+ (middle - 0.5) * 85.0;
	float top_warp = (top_noise - 0.5) * mix(380.0, 1250.0, type)
		+ (middle - 0.5) * mix(150.0, 430.0, type);
	float local_base = max(low_base + base_warp, 520.0);
	float local_top = min(max(low_top + top_warp, local_base + 300.0), 18100.0);

	if (altitude >= local_base && altitude <= local_top && low_mask > 0.002) {
		float h = (altitude - local_base) / max(local_top - local_base, 1.0);
		float profile = cloud_low_vertical_profile(h, type);
		float interior = cloud_low_edge_interior(w, low_n);

		float shape = broad * 0.48 + middle * 0.30
			+ detail * (0.16 * detail_weight)
			+ micro * (0.06 * detail_weight);
		float threshold = mix(0.43, 0.50, smoothstep(0.18, 0.78, type));
		float edge_body = smoothstep(threshold - 0.12, threshold + 0.13, shape);

		// Interior cloud is continuous, not binary. Retain enough modulation for
		// readable volume without opening repeated holes in overcast.
		float interior_body = mix(0.55, 1.08, broad)
			* mix(0.82, 1.08, middle);
		float interior_weight = interior * mix(0.82, 0.48, type);
		float body = mix(edge_body, interior_body, interior_weight) * low_mask;

		if (detail_weight > 0.025) {
			float edge_weight = 1.0 - interior;
			float erosion = (1.0 - detail) * u_detail_strength * detail_weight
				* (0.35 + edge_weight * 0.65);
			erosion += (1.0 - micro) * 0.09 * detail_weight * edge_weight;
			body = max(body - erosion, 0.0);
		}

		float loading = 0.54 + coverage * 0.30 + type * 0.48 + flow.b * 0.16;
		low_density = body * profile * loading;
	}

	float high_density = 0.0;
	float high_base = cloud_high_base_from_weather(w);
	float high_top = cloud_high_top_from_weather(w);
	// The high layer gets a different displacement field and substantially less
	// vertical warp. It remains thin/wispy unless weather explicitly requests
	// an anvil.
	float high_base_warp = (middle - 0.5) * 220.0;
	float high_top_warp = (top_noise - 0.5) * 520.0 + (middle - 0.5) * 170.0;
	float local_high_base = max(high_base + high_base_warp, 7200.0);
	float local_high_top = min(max(high_top + high_top_warp, local_high_base + 650.0), 18200.0);

	if (altitude >= local_high_base && altitude <= local_high_top
			&& high_mask > 0.002 && high_cov > 0.004) {
		float hhi = (altitude - local_high_base)
			/ max(local_high_top - local_high_base, 1.0);
		float profile_hi = cloud_high_vertical_profile(hhi);
		float wispy = texture(u_base_noise,
			q * vec3(0.0000032, 0.000012, 0.0000032) + vec3(0.31, 0.07, 0.53)).r;
		float filament = texture(u_detail_noise,
			q * vec3(0.000026, 0.000008, 0.000026) + vec3(0.61, 0.13, 0.29)).r;
		float interior_hi = cloud_high_edge_interior(w, high_n);
		float hi_shape = wispy * 0.70 + filament * (0.30 * detail_weight)
			+ 0.15 * (1.0 - detail_weight);
		float hi_body = mix(smoothstep(0.36, 0.65, hi_shape),
			mix(0.58, 1.02, wispy), interior_hi * 0.60);
		high_density = high_mask * hi_body * profile_hi
			* mix(0.08, 0.42, high_cov) * (0.82 + type * 0.18);

		// Do not count the same deep convective column as two independent opaque
		// layers where tower and anvil overlap.
		high_density *= 1.0 - clamp(low_density * smoothstep(0.55, 0.90, type)
			* 0.36, 0.0, 0.42);
	}

	return clamp((low_density + high_density) * u_density_scale, 0.0, 1.55);
}

/// Cheap secondary-ray density: exactly the same macro masks and layer
/// boundaries, but no 3D texture fetches.
float cloud_density_light(vec3 p) {
	float r = length(p);
	float altitude = r - u_planet_radius;
	if (altitude < 450.0 || altitude > 18500.0) return 0.0;
	vec3 d = p / max(r, 1.0);
	vec4 w = weather_at(d);
	if (max(max(w.r, w.g), w.b) < 0.006) return 0.0;
	vec2 low_n = cloud_structure_noises(d);
	vec2 high_n = cloud_high_structure_noises(d);
	return cloud_macro_density(w, altitude, low_n, high_n) * u_density_scale;
}

float optical_density(float density) {
	float core = smoothstep(0.12, 0.78, density);
	return density * mix(1.0, u_core_extinction_boost, core);
}

/// Non-uniform macro light march. Uniform four-sample marches alias badly when
/// Helion is near the horizon because their sample spacing can reach many km.
float cloud_sun_optical_depth(vec3 p, vec3 sun_dir) {
	vec2 shell = sphere_hit(p, sun_dir, u_cloud_outer_radius);
	if (shell.y <= 1.0) return 0.0;

	float max_dist = min(shell.y, u_sun_shadow_max_distance);
	float tau = 0.0;
	float previous = 0.0;
	for (int i = 0; i < SUN_SHADOW_STEPS; i++) {
		float u1 = float(i + 1) / float(SUN_SHADOW_STEPS);
		float next_t = max_dist * u1 * u1;
		float segment = max(next_t - previous, 1.0);
		float sample_t = previous + segment * 0.5;
		float d = optical_density(cloud_density_light(p + sun_dir * sample_t));
		tau += d * segment * u_extinction * 0.72;
		previous = next_t;
		if (tau >= 9.0) break;
	}
	return tau;
}

float hg(float g, float mu) {
	float gg = g * g;
	return (1.0 - gg) / (4.0 * PI
		* pow(max(1.0 + gg - 2.0 * g * mu, 1e-4), 1.5));
}

float screen_jitter(vec2 p) {
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

vec3 cloud_multiple_scattering(float sun_tau, float density, float mu, float planet_vis) {
	float order2 = exp(-sun_tau * 0.34);
	float order3 = exp(-sun_tau * 0.11);
	float body = 1.0 - exp(-density * 2.3);
	float angle = 0.72 + 0.28 * clamp(mu * 0.5 + 0.5, 0.0, 1.0);
	float energy = (order2 * 0.15 + order3 * 0.045) * u_multiple_scatter_strength;
	energy *= body * angle * planet_vis;
	return mix(vec3(0.50, 0.60, 0.76), vec3(1.0, 0.91, 0.78),
		smoothstep(-0.12, 0.32, mu)) * energy;
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
	float amount = clamp(1.0 - exp(-seg * air * 1.15e-5), 0.0, 0.78);
	float planet_vis = planet_direct_sun_visibility(probe, sun_dir, u_planet_radius);
	if (planet_vis <= 0.001) return vec4(0.0);
	float mu = dot(normalize(delta), sun_dir);
	vec3 tint = mix(vec3(0.18, 0.31, 0.55), vec3(0.78, 0.69, 0.57),
		smoothstep(-0.12, 0.35, mu));
	float horizon = 1.0 - abs(dot(normalize(probe), normalize(delta)));
	tint *= (0.36 + horizon * 0.32) * planet_vis;
	return vec4(tint, amount * 0.46 * planet_vis);
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

	vec4 view_far = INV_PROJECTION_MATRIX
		* vec4(screen_uv * 2.0 - 1.0, 0.0, 1.0);
	vec3 view_dir = normalize(view_far.xyz / max(abs(view_far.w), 1e-8));
	vec3 rd = normalize((INV_VIEW_MATRIX * vec4(view_dir, 0.0)).xyz);
	vec3 ro = u_camera_world;
	vec3 sun_dir = normalize(u_sun_dir);

	// One representation at every altitude. The former orbit shell was a second
	// cloud system with different density, lighting and depth behavior.
	vec2 outer = sphere_hit(ro, rd, u_cloud_outer_radius);
	if (outer.x > outer.y || outer.y <= 0.0) discard;
	float t0 = max(outer.x, 0.0);
	float t1 = outer.y;

	// The mean planet gives a robust opaque lower bound even when scene depth is
	// absent (for example behind transparent ocean).
	vec2 ground = sphere_hit(ro, rd, u_planet_radius);
	if (ground.x > 0.0) t1 = min(t1, ground.x);

	if (scene_dist < BIG * 0.5) {
		float depth_guard = max(3.0, scene_dist * 2.0e-6);
		t1 = min(t1, max(scene_dist - depth_guard, 0.0));
	}
	if (t1 <= t0 + 1.0) discard;

	float base_dense_step = clamp(u_dense_step, 220.0, 480.0);
	float base_empty_step = clamp(u_empty_step, 700.0, 1500.0);
	float lighting_stride = max(u_lighting_stride, base_dense_step * 4.0);
	float jitter = screen_jitter(FRAGCOORD.xy);
	// Small stochastic offset breaks coherent bands without moving the first
	// sample by nearly a whole cloud step every pixel.
	float t = t0 + base_dense_step * (0.10 + 0.28 * jitter);

	float transmittance = 1.0;
	vec3 radiance = vec3(0.0);
	float optical_t_sum = 0.0;
	float optical_weight = 0.0;
	float mu = dot(rd, sun_dir);
	float phase = hg(0.72, mu) * 0.78 + hg(-0.20, mu) * 0.22;
	phase = min(phase * 4.0 * PI, 4.0);
	float cached_sun_tau = 0.0;
	float cached_sun_t = -BIG;

	// No coarse 3-point "possible cloud" pre-test here. Long horizon rays can
	// cross narrow weather bodies that those samples completely miss.
	for (int i = 0; i < MAX_STEPS; i++) {
		if (t >= t1 || transmittance <= u_transmittance_cutoff) break;

		float distance_lod = smoothstep(28000.0, 170000.0, t);
		float step_scale = mix(1.0, 3.6, distance_lod);
		float dense_step = min(base_dense_step * step_scale, 1250.0);
		float empty_step = min(base_empty_step * step_scale, 4300.0);
		float detail_weight = 1.0 - smoothstep(16000.0, 68000.0, t);

		vec3 p = ro + rd * t;
		float density = cloud_density(p, detail_weight);

		if (density <= 0.0012) {
			// Empty-space acceleration uses the SAME macro density as lighting.
			// A shorter skip and a midpoint probe trade some speed for greatly
			// reduced missed-cloud rectangles/popping.
			float skip_len = min(empty_step, t1 - t);
			float probe_t = t + skip_len * 0.5;
			if (cloud_density_light(ro + rd * probe_t) <= 0.0010) {
				t += skip_len;
				cached_sun_t = -BIG;
				continue;
			}
			t += min(dense_step * 0.55, skip_len * 0.35);
			continue;
		}

		float step_len = min(dense_step, t1 - t);
		float extinction_density = optical_density(density);
		float optical = extinction_density * step_len * u_extinction;
		float step_transmittance = exp(-optical);
		float alpha_step = 1.0 - step_transmittance;
		vec3 sample_pos = p + rd * (step_len * 0.5);

		if (cached_sun_t < -BIG * 0.5
				|| (t - cached_sun_t) >= lighting_stride * step_scale) {
			cached_sun_tau = cloud_sun_optical_depth(sample_pos, sun_dir);
			cached_sun_t = t;
		}

		float sun_tau = cached_sun_tau
			+ extinction_density * u_extinction * min(step_len, 420.0) * 0.18;
		float sun_trans = exp(-sun_tau);
		float planet_vis = planet_direct_sun_visibility(
			sample_pos, sun_dir, u_planet_radius);

		vec3 ambient = planet_sky_irradiance(
			sample_pos, sun_dir, u_planet_radius) * u_ambient_strength;
		float core = smoothstep(0.12, 0.78, density);
		float sky_access = mix(0.20, 0.94, pow(max(sun_trans, 0.0), 0.24));
		sky_access *= mix(1.0, 0.52,
			core * (1.0 - sqrt(max(sun_trans, 0.0))));

		float powder = 1.0 - exp(-density * 3.0);
		vec3 direct = vec3(1.0, 0.91, 0.79) * planet_vis * sun_trans
			* (0.030 + phase * 0.64)
			* (0.54 + powder * 0.46) * u_direct_light_strength;
		vec3 multiple = cloud_multiple_scattering(
			sun_tau, density, mu, planet_vis);
		vec3 sample_light = ambient * sky_access + direct + multiple;

		float electric = lightning_illumination(sample_pos);
		if (electric > 0.001) {
			sample_light += vec3(0.42, 0.62, 1.0) * electric
				* (0.20 + powder * 0.46);
		}

		float contribution = transmittance * alpha_step;
		radiance += contribution * sample_light;
		optical_t_sum += contribution * (t + step_len * 0.5);
		optical_weight += contribution;
		transmittance *= step_transmittance;

		// No "opaque after N metres" shortcut. Beer-Lambert extinction is the
		// only opacity mechanism, avoiding flat cut-out cloud slabs.
		t += step_len;
	}

	float alpha = 1.0 - transmittance;
	if (alpha < 0.002) discard;

	vec3 cloud_color = radiance / max(alpha, 1e-4);
	if (optical_weight > 1e-5) {
		float representative_t = optical_t_sum / optical_weight;
		vec4 aerial = cloud_aerial_fog(
			ro, ro + rd * representative_t, sun_dir);
		cloud_color = mix(cloud_color, aerial.rgb, aerial.a);
	}

	ALBEDO = cloud_color * alpha;
	ALPHA = alpha;
}
