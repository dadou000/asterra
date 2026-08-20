/// Cheap planet-space cloud optical depth shared by terrain, ocean and sky.
///
/// The fullscreen cloud volume cannot participate in Godot's shadow map. Rather
/// than inventing a second procedural cloud field here, this projects the same
/// weather-driven macro structure used by the visible cloud renderer onto three
/// representative cloud shells. Fine 3D erosion is intentionally omitted from
/// the shadow map: it is sub-pixel at long range and would multiply GPU cost.

#include "res://shaders/cloud_structure.gdshaderinc"

uniform sampler2DArray u_cloud_weather_map : filter_linear, repeat_disable;
uniform float u_cloud_weather_face_res = 0.0;
uniform float u_cloud_shadow_strength : hint_range(0.0, 1.0) = 0.96;
uniform float u_cloud_shadow_low_altitude = 2600.0;
uniform float u_cloud_shadow_mid_altitude = 7200.0;
uniform float u_cloud_shadow_high_altitude = 12400.0;

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

float cloud_shadow_layer(vec3 p, vec3 sun_dir, float altitude, int mode) {
	float shell_radius = u_planet_radius + altitude;
	if (length(p) >= shell_radius - 10.0) return 0.0;
	vec2 hit = cloud_shadow_sphere_hit(p, sun_dir, shell_radius);
	if (hit.y <= 0.0) return 0.0;

	vec3 cloud_p = p + sun_dir * hit.y;
	vec3 d = normalize(cloud_p);
	vec4 w = cloud_shadow_weather(d);
	if (max(max(w.r, w.g), w.b) < 0.008) return 0.0;

	vec2 n = cloud_structure_noises(d);
	float low_mask = cloud_low_structure_mask(w, n);
	float high_mask = cloud_high_structure_mask(w, n);
	float type = cloud_type_from_weather(w);

	if (mode == 0) {
		return low_mask * mix(0.48, 1.0, clamp(w.r, 0.0, 1.0));
	}
	if (mode == 1) {
		float tower = smoothstep(0.28, 0.78, type);
		return low_mask * tower * mix(0.40, 1.15, type);
	}
	return high_mask * mix(0.22, 0.82, clamp(w.b, 0.0, 1.0));
}

/// Cloud-only visibility of Helion from an arbitrary atmospheric point. Planet
/// occultation is deliberately handled by planet_direct_sun_visibility() so the
/// two effects remain independent and composable.
float cloud_sun_visibility(vec3 p, vec3 sun_dir) {
	if (u_cloud_weather_face_res < 1.0) return 1.0;
	vec3 s = normalize(sun_dir);
	vec3 up = normalize(p);
	if (dot(up, s) <= -0.08) return 1.0;

	float low = cloud_shadow_layer(p, s, u_cloud_shadow_low_altitude, 0);
	float mid = cloud_shadow_layer(p, s, u_cloud_shadow_mid_altitude, 1);
	float high = cloud_shadow_layer(p, s, u_cloud_shadow_high_altitude, 2);
	// Approximate vertically integrated optical depth. The exponential response
	// produces soft translucent decks and near-black direct-sun shadows under
	// deep convective cores without touching diffuse skylight.
	float tau = low * 2.30 + mid * 2.05 + high * 0.95;
	float transmission = exp(-tau * u_cloud_shadow_strength);
	return clamp(transmission, 0.015, 1.0);
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
