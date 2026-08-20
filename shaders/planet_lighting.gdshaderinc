/// Planet-scale lighting helpers shared by terrain, ocean, clouds and sky.
///
/// Godot's DirectionalLight3D is intentionally unaware of a spherical planet.
/// These functions provide the missing astronomical visibility test so direct
/// sunlight cannot reach a point through Asterra itself.

float planet_horizon_cosine(float radius, float planet_radius) {
	float r = max(radius, planet_radius + 0.01);
	float ratio = clamp(planet_radius / r, 0.0, 1.0);
	return -sqrt(max(1.0 - ratio * ratio, 0.0));
}

float planet_direct_sun_visibility(vec3 p, vec3 sun_dir, float planet_radius) {
	vec3 s = normalize(sun_dir);
	float r = length(p);
	if (r < 1.0) return 0.0;
	vec3 up = p / r;

	// Lift points that numerically sit a few centimetres inside the reference
	// sphere. This avoids false self-intersections on sea level.
	vec3 ro = p;
	if (r <= planet_radius + 0.5) {
		ro = up * (planet_radius + 0.5);
		r = planet_radius + 0.5;
	}

	// Exact ray/sphere occultation by the mean planetary body.
	float b = dot(ro, s);
	float c = dot(ro, ro) - planet_radius * planet_radius;
	float disc = b * b - c;
	if (disc > 0.0) {
		float t_near = -b - sqrt(disc);
		if (t_near > 1.0) return 0.0;
	}

	// A small angular transition approximates the finite solar disc and removes
	// numerical flicker exactly at the geometric horizon. At altitude the
	// geometric horizon correctly drops below the local tangent plane.
	float horizon = planet_horizon_cosine(r, planet_radius);
	float mu = dot(up, s);
	const float SOLAR_EDGE = 0.0062;
	return smoothstep(horizon - SOLAR_EDGE, horizon + SOLAR_EDGE, mu);
}

/// Cheap local hemispherical sky irradiance. This is deliberately modest: the
/// direct DirectionalLight remains the main daytime energy source. The purpose
/// here is to replace global Godot ambient on planetary materials with a local
/// term that follows the terminator and leaves night genuinely dark.
vec3 planet_sky_irradiance(vec3 p, vec3 sun_dir, float planet_radius) {
	vec3 up = normalize(p);
	float mu = dot(up, normalize(sun_dir));
	float day = smoothstep(-0.08, 0.20, mu);
	float twilight = smoothstep(-0.22, -0.015, mu) * (1.0 - smoothstep(-0.015, 0.16, mu));
	float high_sun = smoothstep(0.08, 0.62, mu);

	vec3 night = vec3(0.0007, 0.0011, 0.0024);
	vec3 dusk = vec3(0.022, 0.030, 0.050) * twilight;
	vec3 blue_sky = mix(vec3(0.070, 0.105, 0.170), vec3(0.150, 0.205, 0.300), high_sun) * day;
	return night + dusk + blue_sky;
}
