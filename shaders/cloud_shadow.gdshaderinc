/// Planet-space cloud optical depth shared by terrain, ocean and atmosphere.
///
/// The fullscreen cloud renderer cannot cast a Godot shadow map. Instead we
/// integrate the same weather-driven macro density used by the visible volume
/// along the ray toward Helion. This is intentionally lower frequency than the
/// visible 3D erosion, but it uses the same low/high layer heights and profiles.
///
/// Previous versions projected three fixed shells (2.6/7.2/12.4 km). That made
/// shadows detach from clouds whenever their weather-defined base/top differed,
/// and counted deep convection more than once. This integral removes that class
/// of mismatch.

#include "res://shaders/cloud_structure.gdshaderinc"

uniform sampler2DArray u_cloud_weather_map : filter_linear, repeat_disable;
uniform float u_cloud_weather_face_res = 0.0;
uniform float u_cloud_shadow_strength : hint_range(0.0, 2.0) = 1.0;
uniform float u_cloud_shadow_extinction = 0.00032;
uniform float u_cloud_shadow_outer_altitude = 18500.0;
uniform float u_cloud_shadow_max_distance = 48000.0;

const int CLOUD_SHADOW_STEPS = 8;

vec3 cloud_shadow_face_uv(vec3 dir) {
	vec3 d = normalize(dir);
	vec3 ad = abs(d);
	int face = 0;
	vec3 axis = vec3(1.0, 0.0, 0.0);
	vec3 right = vec3(0.0, 0.0, -1.0);
	vec3 up = vec3(0.0, 1.0, 0.0);

	if (ad.x >= ad.y && ad.x >= ad.z) {
		if (d.x >= 0.0) {
			face = 0; axis = vec3(1.0, 0.0, 0.0); right = vec3(0.0, 0.0, -1.0); up = vec3(0.0, 1.0, 0.0);
		} else {
			face = 1; axis = vec3(-1.0, 0.0, 0.0); right = vec3(0.0, 0.0, 1.0); up = vec3(0.0, 1.0, 0.0);
		}
	} else if (ad.y >= ad.z) {
		if (d.y >= 0.0) {
			face = 2; axis = vec3(0.0, 1.0, 0.0); right = vec3(-1.0, 0.0, 0.0); up = vec3(0.0, 0.0, 1.0);
		} else {
			face = 3; axis = vec3(0.0, -1.0, 0.0); right = vec3(-1.0, 0.0, 0.0); up = vec3(0.0, 0.0, -1.0);
		}
	} else {
		if (d.z >= 0.0) {
			face = 4; axis = vec3(0.0, 0.0, 1.0); right = vec3(1.0, 0.0, 0.0); up = vec3(0.0, 1.0, 0.0);
		} else {
			face = 5; axis = vec3(0.0, 0.0, -1.0); right = vec3(-1.0, 0.0, 0.0); up = vec3(0.0, 1.0, 0.0);
		}
	}

	float denom = max(dot(axis, d), 1e-8);
	float q = PI * 0.25;
	float u = atan(dot(right, d) / denom) / q;
	float v = atan(dot(up, d) / denom) / q;
	return vec3(u * 0.5 + 0.5, v * 0.5 + 0.5, float(face));
}

vec3 cloud_shadow_array_coord(vec3 dir) {
	vec3 fuv = cloud_shadow_face_uv(dir);
	float texture_res = u_cloud_weather_face_res + 2.0;
	vec2 uv = (fuv.xy * u_cloud_weather_face_res + vec2(1.0)) / max(texture_res, 1.0);
	return vec3(uv, fuv.z);
}

vec4 cloud_shadow_weather(vec3 dir) {
	return texture(u_cloud_weather_map, cloud_shadow_array_coord(normalize(dir)));
}

vec2 cloud_shadow_sphere_hit(vec3 ro, vec3 rd, float radius) {
	float b = dot(ro, rd);
	float c = dot(ro, ro) - radius * radius;
	float h = b * b - c;
	if (h < 0.0) return vec2(1e20, -1e20);
	float s = sqrt(h);
	return vec2(-b - s, -b + s);
}

float cloud_shadow_macro_density(vec3 p) {
	float r = length(p);
	float altitude = r - u_planet_radius;
	if (altitude < 450.0 || altitude > u_cloud_shadow_outer_altitude) return 0.0;

	vec3 d = p / max(r, 1.0);
	vec4 w = cloud_shadow_weather(d);
	if (max(max(w.r, w.g), w.b) < 0.006) return 0.0;

	vec2 low_n = cloud_structure_noises(d);
	vec2 high_n = cloud_high_structure_noises(d);
	return cloud_macro_density(w, altitude, low_n, high_n);
}

/// Cloud-only Helion transmission. The quadratic segment layout concentrates
/// samples near the receiver (where projected shadow position matters most) and
/// grows them with distance. This is substantially more stable at grazing sun
/// angles than four uniformly spaced samples across an entire spherical shell.
float cloud_sun_visibility(vec3 p, vec3 sun_dir) {
	if (u_cloud_weather_face_res < 1.0) return 1.0;

	vec3 s = normalize(sun_dir);
	vec3 up = normalize(p);
	// Planet occultation is handled independently by planet_lighting. Avoid
	// wasting cloud samples on the deep night side.
	if (dot(up, s) <= -0.12) return 1.0;

	float outer_radius = u_planet_radius + u_cloud_shadow_outer_altitude;
	vec2 shell = cloud_shadow_sphere_hit(p, s, outer_radius);
	if (shell.y <= 1.0) return 1.0;

	float max_dist = min(shell.y, u_cloud_shadow_max_distance);
	float tau = 0.0;
	float previous = 0.0;

	for (int i = 0; i < CLOUD_SHADOW_STEPS; i++) {
		float u1 = float(i + 1) / float(CLOUD_SHADOW_STEPS);
		float next_t = max_dist * u1 * u1;
		float segment = max(next_t - previous, 1.0);
		float sample_t = previous + segment * 0.5;
		float density = cloud_shadow_macro_density(p + s * sample_t);
		tau += density * segment * u_cloud_shadow_extinction;
		previous = next_t;
		if (tau >= 9.0) break;
	}

	return exp(-tau * u_cloud_shadow_strength);
}

float cloud_surface_sun_visibility(vec3 surface_p, vec3 sun_dir) {
	vec3 p = normalize(surface_p) * max(length(surface_p), u_planet_radius + 1.0);
	return cloud_sun_visibility(p, sun_dir);
}

float cloud_shadow_amount(vec3 p, vec3 sun_dir) {
	return 1.0 - cloud_sun_visibility(p, sun_dir);
}

float cloud_surface_shadow_amount(vec3 p, vec3 sun_dir) {
	return 1.0 - cloud_surface_sun_visibility(p, sun_dir);
}
