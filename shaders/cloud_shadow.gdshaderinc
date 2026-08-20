/// Cheap planet-space cloud occlusion shared by terrain, ocean and atmosphere.
///
/// The visible cloud renderer is a fullscreen volume and cannot enter Godot's
/// directional shadow map. This helper therefore projects the dynamic weather
/// envelope toward Helion, then shapes it with a cheap procedural field so the
/// footprint follows cloud bodies instead of looking like a uniform weather mask.

uniform sampler2DArray u_cloud_weather_map : filter_linear, repeat_disable;
uniform float u_cloud_weather_face_res = 0.0;
uniform float u_cloud_shadow_strength : hint_range(0.0, 1.0) = 0.98;
uniform float u_cloud_shadow_low_altitude = 2800.0;
uniform float u_cloud_shadow_mid_altitude = 8200.0;
uniform float u_cloud_shadow_high_altitude = 13200.0;

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

float cloud_shadow_hash(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.yzx + 33.33);
	return fract((p.x + p.y) * p.z);
}

float cloud_shadow_vnoise(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(
		mix(mix(cloud_shadow_hash(i), cloud_shadow_hash(i + vec3(1.0, 0.0, 0.0)), f.x),
			mix(cloud_shadow_hash(i + vec3(0.0, 1.0, 0.0)), cloud_shadow_hash(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
		mix(mix(cloud_shadow_hash(i + vec3(0.0, 0.0, 1.0)), cloud_shadow_hash(i + vec3(1.0, 0.0, 1.0)), f.x),
			mix(cloud_shadow_hash(i + vec3(0.0, 1.0, 1.0)), cloud_shadow_hash(i + vec3(1.0, 1.0, 1.0)), f.x), f.y),
		f.z);
}

vec3 cloud_shadow_space(vec3 p) {
	return vec3(
		dot(p, vec3(0.788, 0.326, -0.522)),
		dot(p, vec3(-0.211, 0.943, 0.259)),
		dot(p, vec3(0.578, -0.094, 0.811))
	);
}

float cloud_shadow_layer(vec3 p, vec3 sun_dir, float altitude, int mode) {
	float shell_radius = u_planet_radius + altitude;
	if (length(p) >= shell_radius - 20.0) return 0.0;
	vec2 hit = cloud_shadow_sphere_hit(p, sun_dir, shell_radius);
	if (hit.y <= 0.0) return 0.0;

	vec3 cloud_p = p + sun_dir * hit.y;
	vec4 w = cloud_shadow_weather(normalize(cloud_p));
	float coverage = clamp(max(w.r, w.b * 0.72) + w.g * 0.12, 0.0, 1.0);
	if (coverage < 0.01) return 0.0;

	vec3 q = cloud_shadow_space(cloud_p + vec3(TIME * 13.0, 0.0, TIME * -8.0));
	float broad = cloud_shadow_vnoise(q * 0.0000032 + vec3(3.7, 9.1, 1.4));
	float middle = cloud_shadow_vnoise(q * 0.000012 + vec3(7.3, 2.1, 5.9));
	float overcast = smoothstep(0.68, 0.95, w.r);
	float shape = broad * 0.68 + middle * 0.32 + w.g * 0.10 + overcast * 0.16;
	float threshold = mix(0.78, 0.37, coverage);
	float body = smoothstep(threshold, min(threshold + 0.14, 0.98), shape);
	float high_veil = smoothstep(0.22, 0.85, w.b) * smoothstep(0.42, 0.72, broad) * 0.48;

	if (mode == 0) return body * mix(0.70, 1.0, w.r);
	if (mode == 1) return body * smoothstep(0.08, 0.72, w.g) * 1.18;
	return max(high_veil, body * w.b * 0.28);
}

/// Visibility of Helion from an arbitrary atmospheric point.
float cloud_sun_visibility(vec3 p, vec3 sun_dir) {
	if (u_cloud_weather_face_res < 1.0) return 1.0;
	vec3 s = normalize(sun_dir);
	vec3 up = normalize(p);
	// Deep night has no direct sun to shadow. Near the terminator the planet/light
	// geometry handles the extinction while this function only adds cloud cover.
	if (dot(up, s) <= -0.04) return 1.0;

	float low = cloud_shadow_layer(p, s, u_cloud_shadow_low_altitude, 0);
	float mid = cloud_shadow_layer(p, s, u_cloud_shadow_mid_altitude, 1);
	float high = cloud_shadow_layer(p, s, u_cloud_shadow_high_altitude, 2);
	float optical = low * 2.25 + mid * 1.85 + high * 1.10;
	float occlusion = 1.0 - exp(-optical);
	return clamp(1.0 - occlusion * u_cloud_shadow_strength, 0.02, 1.0);
}

float cloud_surface_sun_visibility(vec3 surface_p, vec3 sun_dir) {
	vec3 mean_surface = normalize(surface_p) * (u_planet_radius + 2.0);
	return cloud_sun_visibility(mean_surface, sun_dir);
}

float cloud_shadow_amount(vec3 p, vec3 sun_dir) {
	return 1.0 - cloud_sun_visibility(p, sun_dir);
}

float cloud_surface_shadow_amount(vec3 p, vec3 sun_dir) {
	return 1.0 - cloud_surface_sun_visibility(p, sun_dir);
}
