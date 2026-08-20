shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, depth_test_disabled, cull_disabled;
/// Asterra volumetric clouds.
///
/// The dynamic WeatherSystem owns large-scale organisation and storm events.
/// Generated 3D noise only turns that physical envelope into cloud morphology.
/// Direct Helion light is blocked by the planet and by cloud water itself;
/// electrical discharges inject short-lived emissive light into storm volumes.

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
uniform float u_extinction = 0.00082;
uniform float u_density_scale = 1.15;
uniform float u_wind_speed = 1.0;
uniform float u_detail_strength = 0.27;

// xyz = canonical planet-space flash position, w = instantaneous HDR energy.
uniform vec4 u_lightning0 = vec4(0.0);
uniform vec4 u_lightning1 = vec4(0.0);
uniform vec4 u_lightning2 = vec4(0.0);
uniform vec4 u_lightning3 = vec4(0.0);

const int MAX_STEPS = 60;
const float BIG = 1.0e20;

void vertex() {
	POSITION = vec4(VERTEX.xy, 1.0, 1.0);
}

vec2 sphere_hit(vec3 ro, vec3 rd, float radius) {
	float b = dot(ro, rd);
	float c = dot(ro, ro) - radius * radius;
	float h = b * b - c;
	if (h < 0.0) {
		return vec2(BIG, -BIG);
	}
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
	float base = smoothstep(0.0, mix(0.060, 0.020, convective), h01);
	float cap = 1.0 - smoothstep(mix(0.70, 0.91, convective), 1.0, h01);
	float deck = 1.0 - h01 * 0.10;
	float tower = smoothstep(0.015, 0.18, h01) * (1.0 - 0.06 * h01);
	return base * cap * mix(deck, tower, convective);
}

vec3 cloud_space(vec3 p) {
	return vec3(
		dot(p, vec3(0.788, 0.326, -0.522)),
		dot(p, vec3(-0.211, 0.943, 0.259)),
		dot(p, vec3(0.578, -0.094, 0.811))
	);
}

float cloud_density(vec3 p, bool lighting_sample) {
	float r = length(p);
	float altitude = r - u_planet_radius;
	if (altitude < 500.0 || altitude > 18500.0) {
		return 0.0;
	}
	vec3 d = p / max(r, 1.0);
	vec4 w = weather_at(d);
	vec4 flow_data = wind_at(d);
	float coverage = w.r;
	float convective = w.g;
	float high_cov = w.b;
	if (max(max(coverage, convective), high_cov) < 0.010) {
		return 0.0;
	}

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
	float detail_n = texture(u_detail_noise, q * 0.000052 + vec3(0.17, 0.43, 0.71)).r;

	float low_density = 0.0;
	if (altitude >= cloud_base && altitude <= low_top) {
		float h01 = remap01(altitude, cloud_base, low_top);
		float profile = vertical_low_profile(h01, convective);
		float overcast = smoothstep(0.68, 0.96, coverage);
		float threshold = mix(0.73, 0.36, coverage);
		threshold -= convective * (0.08 + 0.11 * h01);
		float tower_body = mix(0.0, 0.12 * smoothstep(0.20, 0.78, h01), convective);
		float shape = broad_n * 0.58 + middle_n * 0.29 + detail_n * 0.13 + overcast * 0.12 + tower_body;
		float body = smoothstep(threshold, min(threshold + 0.17, 0.98), shape);
		if (!lighting_sample) {
			float edge_weight = 1.0 - body;
			float erosion = (1.0 - detail_n) * u_detail_strength * edge_weight * (1.0 - convective * 0.68);
			body = max(body - erosion, 0.0);
		}
		low_density = body * profile;
		low_density *= 0.70 + convective * 0.72 + flow_data.b * 0.26;
	}

	float high_start = mix(8200.0, 9800.0, 1.0 - high_cov);
	float high_end = 17000.0;
	float high_density = 0.0;
	if (altitude >= high_start && altitude <= high_end && high_cov > 0.01) {
		float hhi = remap01(altitude, high_start, high_end);
		float profile_hi = smoothstep(0.0, 0.10, hhi) * (1.0 - smoothstep(0.72, 1.0, hhi));
		vec3 high_p = q * vec3(0.0000017, 0.0000027, 0.0000017);
		float wispy = texture(u_base_noise, high_p + vec3(0.31, 0.07, 0.53)).r;
		float filaments = texture(u_detail_noise, q * 0.000020 + vec3(0.61, 0.13, 0.29)).r;
		float hi_threshold = mix(0.84, 0.39, high_cov);
		float hi = smoothstep(hi_threshold, min(hi_threshold + 0.17, 0.98), wispy * 0.82 + filaments * 0.18 + convective * 0.06);
		high_density = hi * profile_hi * mix(0.18, 0.55, high_cov);
	}

	return clamp((low_density + high_density) * u_density_scale, 0.0, 1.7);
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
	if (hit.x > 0.0 && hit.y > 0.0) {
		return 0.0;
	}
	return 1.0;
}

float lightning_one(vec3 p, vec4 flash) {
	if (flash.w <= 0.001) {
		return 0.0;
	}
	float radius = mix(4200.0, 26000.0, clamp(flash.w / 12.0, 0.0, 1.0));
	float x = length(p - flash.xyz) / max(radius, 1.0);
	return flash.w * exp(-x * x * 2.15);
}

float lightning_illumination(vec3 p) {
	return lightning_one(p, u_lightning0)
		+ lightning_one(p, u_lightning1)
		+ lightning_one(p, u_lightning2)
		+ lightning_one(p, u_lightning3);
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

	vec2 outer = sphere_hit(ro, rd, u_cloud_outer_radius);
	if (outer.x > outer.y || outer.y <= 0.0) {
		discard;
	}

	float t0 = max(outer.x, 0.0);
	float t1 = outer.y;
	vec2 ground = sphere_hit(ro, rd, u_planet_radius);
	if (ground.x > 0.0) {
		t1 = min(t1, ground.x);
	}
	t1 = min(t1, scene_dist);
	if (t1 <= t0 + 1.0) {
		discard;
	}

	float seg = t1 - t0;
	vec4 w0 = weather_at(normalize(ro + rd * (t0 + seg * 0.20)));
	vec4 w1 = weather_at(normalize(ro + rd * (t0 + seg * 0.50)));
	vec4 w2 = weather_at(normalize(ro + rd * (t0 + seg * 0.80)));
	float possible = max(max(max(w0.r, w0.g), w0.b), max(max(w1.r, w1.g), max(w1.b, max(max(w2.r, w2.g), w2.b))));
	if (possible < 0.010) {
		discard;
	}

	int steps = int(clamp(ceil(seg / 420.0), 22.0, float(MAX_STEPS)));
	float ds = seg / float(steps);
	float t = t0 + ds * screen_jitter(FRAGCOORD.xy);
	float transmittance = 1.0;
	vec3 radiance = vec3(0.0);
	vec3 sun_dir = normalize(u_sun_dir);
	float mu = dot(rd, sun_dir);
	float phase = hg(0.72, mu) * 0.78 + hg(-0.20, mu) * 0.22;
	phase *= 4.0 * PI;

	for (int i = 0; i < MAX_STEPS; i++) {
		if (i >= steps || transmittance < 0.008) {
			break;
		}
		vec3 p = ro + rd * t;
		float density = cloud_density(p, false);
		if (density > 0.002) {
			float optical = density * ds * u_extinction;
			float alpha_step = 1.0 - exp(-optical);

			// Long sunward probe chain makes the lower half of deep clouds genuinely
			// shadowed instead of treating every sample as if it were near an edge.
			float ld0 = cloud_density(p + sun_dir * 550.0, true);
			float ld1 = cloud_density(p + sun_dir * 1750.0, true);
			float ld2 = cloud_density(p + sun_dir * 4150.0, true);
			float ld3 = cloud_density(p + sun_dir * 8950.0, true);
			float light_tau = (ld0 * 550.0 + ld1 * 1200.0 + ld2 * 2400.0 + ld3 * 4800.0)
				* u_extinction * 1.12;
			float sun_trans = exp(-light_tau);
			float sun_visible = planet_sun_visibility(p, sun_dir);
			vec3 up = normalize(p);
			float sun_elev = dot(up, sun_dir);
			float twilight = smoothstep(-0.22, 0.10, sun_elev);

			// Night side receives only a tiny sky floor. Direct sunlight is zero when
			// the planet blocks Helion, including deep into the night hemisphere.
			vec3 night_ambient = vec3(0.0006, 0.0009, 0.0018);
			vec3 day_ambient = vec3(0.34, 0.45, 0.61);
			vec3 ambient = mix(night_ambient, day_ambient, twilight);
			float powder = 1.0 - exp(-density * 3.4);
			vec3 direct = vec3(1.0, 0.92, 0.80) * sun_visible * sun_trans
				* (0.10 + phase * 0.74) * (0.64 + powder * 0.46);
			float sky_occlusion = mix(0.28, 0.92, sqrt(max(sun_trans, 0.0)));
			vec3 sample_light = ambient * sky_occlusion + direct;

			// Lightning is the exception: it is locally emissive and can light a
			// cumulonimbus from within even on the dark side of Asterra.
			float electric = lightning_illumination(p);
			if (electric > 0.001) {
				vec3 electric_color = vec3(0.42, 0.62, 1.0);
				sample_light += electric_color * electric * (0.20 + powder * 0.46);
			}

			radiance += transmittance * alpha_step * sample_light;
			transmittance *= 1.0 - alpha_step;
		}
		t += ds;
	}

	float alpha = 1.0 - transmittance;
	if (alpha < 0.002) {
		discard;
	}
	ALBEDO = radiance / max(alpha, 1e-4);
	ALPHA = alpha;
}
