/// Planet-scale lighting helpers shared by terrain, ocean and sky.
///
/// Godot's DirectionalLight3D is intentionally unaware of a spherical planet.
/// These functions provide astronomical visibility and the shared atmospheric
/// solar transmittance lookup used by all planetary render paths.

global uniform sampler2D asterra_transmittance_lut;
global uniform vec3 asterra_sun_dir;

/// Apparent angular radius of Helion in radians. Runtime code derives this from
/// the physical stellar radius and Asterra-Helion distance. The default is a
/// Sol-radius source at one astronomical unit (~0.2667 degrees radius).
uniform float u_helion_angular_radius_rad = 0.00465475;

float planet_horizon_cosine(float radius, float planet_radius) {
	float r = max(radius, planet_radius + 0.01);
	float ratio = clamp(planet_radius / r, 0.0, 1.0);
	return -sqrt(max(1.0 - ratio * ratio, 0.0));
}

/// Visible fraction of Helion's finite stellar disc at Asterra's geometric
/// horizon. The circle-segment expression approximates the planetary limb as a
/// straight line across the tiny apparent stellar disc.
float planet_direct_sun_visibility(vec3 p, vec3 sun_dir, float planet_radius) {
	vec3 s = normalize(sun_dir);
	float r = length(p);
	if (r < 1.0) return 0.0;
	vec3 up = p / r;
	if (r <= planet_radius + 0.5) r = planet_radius + 0.5;

	float horizon_cos = planet_horizon_cosine(r, planet_radius);
	float mu = clamp(dot(up, s), -1.0, 1.0);
	float horizon_zenith = acos(clamp(horizon_cos, -1.0, 1.0));
	float sun_zenith = acos(mu);
	float clearance = horizon_zenith - sun_zenith;
	float angular_radius = max(u_helion_angular_radius_rad, 1e-7);
	if (clearance <= -angular_radius) return 0.0;
	if (clearance >= angular_radius) return 1.0;

	float x = clamp(clearance / angular_radius, -1.0, 1.0);
	float root = sqrt(max(1.0 - x * x, 0.0));
	return (acos(-x) + x * root) / PI;
}

/// Shared atmosphere-to-space transmittance. X is solar zenith cosine and Y is
/// altitude. The LUT is generated once per resident body's radius/atmosphere,
/// so per-pixel secondary solar ray marches collapse to one filtered fetch.
vec3 planet_solar_transmittance(vec3 p, vec3 sun_dir, float planet_radius,
		float atmosphere_height) {
	float radius = length(p);
	float height = clamp(radius - planet_radius, 0.0, atmosphere_height);
	vec3 up = p / max(radius, 1.0);
	float mu = clamp(dot(up, normalize(sun_dir)), -1.0, 1.0);
	vec2 uv = vec2(mu * 0.5 + 0.5,
		height / max(atmosphere_height, 1.0));
	return texture(asterra_transmittance_lut, uv).rgb
		* planet_direct_sun_visibility(p, sun_dir, planet_radius);
}

/// Cheap local hemispherical sky irradiance. This replaces global Godot ambient
/// on planetary materials with a local term that follows the terminator and
/// leaves night genuinely dark.
vec3 planet_sky_irradiance(vec3 p, vec3 sun_dir, float planet_radius) {
	vec3 up = normalize(p);
	float mu = dot(up, normalize(sun_dir));
	float day = smoothstep(-0.08, 0.20, mu);
	float twilight = smoothstep(-0.22, -0.015, mu) * (1.0 - smoothstep(-0.015, 0.16, mu));
	float high_sun = smoothstep(0.08, 0.62, mu);

	vec3 night = vec3(0.0006, 0.0010, 0.0022);
	vec3 dusk = vec3(0.017, 0.022, 0.037) * twilight;
	vec3 blue_sky = mix(vec3(0.023, 0.034, 0.058), vec3(0.048, 0.066, 0.102), high_sun) * day;
	return night + dusk + blue_sky;
}
