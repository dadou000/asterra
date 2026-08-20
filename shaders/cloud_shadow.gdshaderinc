/// Cheap planet-space cloud occlusion shared by terrain, ocean and atmosphere.
///
/// The visible cloud renderer is a fullscreen volume and cannot enter Godot's
/// directional shadow map. This helper therefore projects the dynamic weather
/// envelope onto a few representative cloud shells along Helion. Surface shaders
/// use the result only in their `light()` function, so clouds block direct sun
/// rather than painting the material/albedo darker.

uniform sampler2DArray u_cloud_weather_map : filter_linear, repeat_disable;
uniform float u_cloud_weather_face_res = 0.0;
uniform float u_cloud_shadow_strength : hint_range(0.0, 1.0) = 0.96;
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

float cloud_shadow_layer(vec3 p, vec3 sun_dir, float altitude, int mode) {
	float shell_radius = u_planet_radius + altitude;
	if (length(p) >= shell_radius - 20.0) return 0.0;
	vec2 hit = cloud_shadow_sphere_hit(p, sun_dir, shell_radius);
	if (hit.y <= 0.0) return 0.0;

	vec4 w = cloud_shadow_weather(normalize(p + sun_dir * hit.y));
	float low = smoothstep(0.16, 0.82, w.r) * mix(0.70, 1.0, w.g);
	float convective = smoothstep(0.12, 0.78, max(w.g, w.r * 0.28));
	float high = smoothstep(0.12, 0.80, w.b) * 0.78;
	if (mode == 0) return low;
	if (mode == 1) return convective;
	return high;
}

/// Visibility of Helion from an arbitrary atmospheric point.
float cloud_sun_visibility(vec3 p, vec3 sun_dir) {
	if (u_cloud_weather_face_res < 1.0) return 1.0;
	vec3 s = normalize(sun_dir);
	vec3 up = normalize(p);
	// The planet itself owns the night side; do not fabricate a cloud shadow there.
	if (dot(up, s) <= -0.04) return 1.0;

	float low = cloud_shadow_layer(p, s, u_cloud_shadow_low_altitude, 0);
	float mid = cloud_shadow_layer(p, s, u_cloud_shadow_mid_altitude, 1);
	float high = cloud_shadow_layer(p, s, u_cloud_shadow_high_altitude, 2);

	// Use a soft union rather than max() so overlapping low/mid/high decks become
	// genuinely optically thick while isolated wisps retain partial transmission.
	float optical = low * 1.85 + mid * 1.55 + high * 0.95;
	float occlusion = 1.0 - exp(-optical);
	return clamp(1.0 - occlusion * u_cloud_shadow_strength, 0.035, 1.0);
}

/// Surface visibility is projected from the mean sphere, not actual terrain
/// elevation. That keeps the cloud footprint geographical and prevents mountain
/// contours from moving the shadow lookup between fixed cloud shells.
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
