/// Aerial perspective: the air standing between the camera and a surface.
/// Geometry keeps a short primary integration, while solar-path extinction is
/// shared through the planet transmittance LUT.

uniform float u_planet_radius = 1000000.0;
uniform float u_atmosphere_height = 60000.0;
uniform vec3 u_sun_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_rayleigh_coeff = vec3(5.5e-6, 13.0e-6, 22.4e-6);
uniform float u_mie_coeff = 21e-6;
uniform float u_rayleigh_scale_height = 8000.0;
uniform float u_mie_scale_height = 1200.0;
uniform float u_mie_g = 0.758;
uniform vec3 u_ozone_coeff = vec3(0.650e-6, 1.881e-6, 0.085e-6);
uniform float u_ozone_centre = 25000.0;
uniform float u_ozone_width = 15000.0;
uniform float u_sun_intensity = 5.0265;
uniform float u_aerial_strength : hint_range(0.0, 2.0) = 0.62;

#include "res://shaders/planet_lighting.gdshaderinc"

const int AP_STEPS = 4;

float atmo_ozone_density(float height) {
	return max(1.0 - abs(height - u_ozone_centre) / u_ozone_width, 0.0);
}

float atmo_mie_phase(float mu, float g) {
	float gg = g * g;
	return (3.0 * (1.0 - gg) * (1.0 + mu * mu))
		/ (8.0 * PI * (2.0 + gg) * pow(max(1.0 + gg - 2.0 * g * mu, 1e-4), 1.5));
}

vec2 atmo_sphere_hit(vec3 origin, vec3 dir, float radius) {
	float b = dot(dir, origin);
	float c = dot(origin, origin) - radius * radius;
	float disc = b * b - c;
	if (disc < 0.0) return vec2(1e9, -1e9);
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
}

void aerial_perspective(vec3 cam, vec3 frag, out vec3 transmittance,
		out vec3 inscatter) {
	transmittance = vec3(1.0);
	inscatter = vec3(0.0);
	vec3 delta = frag - cam;
	float seg = length(delta);
	if (seg < 25.0) return;
	vec3 dir = delta / seg;
	float top = u_planet_radius + u_atmosphere_height;
	vec2 hit = atmo_sphere_hit(cam, dir, top);
	float t0 = max(hit.x, 0.0);
	float t1 = min(hit.y, seg);
	if (t1 <= t0) return;

	float step_len = (t1 - t0) / float(AP_STEPS);
	float od_rlh = 0.0;
	float od_mie = 0.0;
	float od_ozo = 0.0;
	vec3 total_rlh = vec3(0.0);
	vec3 total_mie = vec3(0.0);
	float t = t0;
	vec3 sun_dir = normalize(u_sun_dir);

	for (int i = 0; i < AP_STEPS; i++) {
		vec3 pos = cam + dir * (t + step_len * 0.5);
		float height = max(length(pos) - u_planet_radius, 0.0);
		float step_rlh = exp(-height / u_rayleigh_scale_height) * step_len;
		float step_mie = exp(-height / u_mie_scale_height) * step_len;
		od_rlh += step_rlh;
		od_mie += step_mie;
		od_ozo += atmo_ozone_density(height) * step_len;

		vec3 sun_transmittance = planet_solar_transmittance(
			pos, sun_dir, u_planet_radius, u_atmosphere_height);
		if (dot(sun_transmittance, vec3(1.0)) > 1e-5) {
			// Camera-to-sample optical depth is still accumulated locally. Only the
			// expensive sample-to-sun half of the integral comes from the LUT.
			vec3 view_attn = exp(-(u_mie_coeff * od_mie
				+ u_rayleigh_coeff * od_rlh
				+ u_ozone_coeff * od_ozo));
			vec3 attn = view_attn * sun_transmittance;
			total_rlh += step_rlh * attn;
			total_mie += step_mie * attn;
		}
		t += step_len;
	}

	float mu = dot(dir, sun_dir);
	float phase_rlh = 3.0 / (16.0 * PI) * (1.0 + mu * mu);
	float phase_mie = atmo_mie_phase(mu, u_mie_g);
	inscatter = u_sun_intensity * u_aerial_strength
		* (phase_rlh * u_rayleigh_coeff * total_rlh
			+ phase_mie * u_mie_coeff * total_mie);
	transmittance = exp(
		-(u_rayleigh_coeff * od_rlh + u_mie_coeff * od_mie
			+ u_ozone_coeff * od_ozo));
}
