shader_type sky;
/// Physically-inspired single-scatter Rayleigh + Mie atmosphere.

uniform vec3 u_up = vec3(0.0, 1.0, 0.0);
uniform float u_camera_height = 0.0;
uniform float u_planet_radius = 1000000.0;
uniform float u_atmosphere_radius = 1060000.0;
uniform vec3 u_rayleigh_coeff = vec3(5.5e-6, 13.0e-6, 22.4e-6);
uniform float u_mie_coeff = 21e-6;
uniform float u_rayleigh_scale_height = 8000.0;
uniform float u_mie_scale_height = 1200.0;
uniform float u_mie_g = 0.72;
/// Ozone absorbs in the Chappuis band without scattering, so it only bites where
/// the slant path is long. That is what keeps a twilight sky and a viewed-from-
/// space limb deep blue instead of letting them wash to grey.
uniform vec3 u_ozone_coeff = vec3(0.650e-6, 1.881e-6, 0.085e-6);
uniform float u_ozone_centre = 25000.0;
uniform float u_ozone_width = 15000.0;
/// See Main.solar_irradiance. Kept identical to the aerial-perspective value so
/// the sky and the haze in front of the ground are lit by the same star.
uniform float u_sun_intensity = 5.0265;

#include "res://shaders/planet_lighting.gdshaderinc"

const int PRIMARY_STEPS = 6;
const int LIGHT_STEPS = 2;

vec2 sphere_intersect(vec3 origin, vec3 dir, float radius) {
	float b = dot(dir, origin);
	float c = dot(origin, origin) - radius * radius;
	float disc = b * b - c;
	if (disc < 0.0) return vec2(1e9, -1e9);
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
}

float ozone_density(float height) {
	return max(1.0 - abs(height - u_ozone_centre) / u_ozone_width, 0.0);
}

/// Cornette-Shanks: keeps the aerosol lobe pointing at the sun -- which is what
/// gives the limb its bright forward-scattered side -- while staying finite
/// enough not to draw a flare around the disc.
float mie_phase(float mu, float g) {
	float gg = g * g;
	return (3.0 * (1.0 - gg) * (1.0 + mu * mu))
		/ (8.0 * PI * (2.0 + gg) * pow(max(1.0 + gg - 2.0 * g * mu, 1e-4), 1.5));
}

float ray_jitter(vec3 d) {
	return fract(sin(dot(d, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
}

vec3 scatter(vec3 dir, vec3 origin, vec3 sun_dir) {
	vec2 atmo_hit = sphere_intersect(origin, dir, u_atmosphere_radius);
	if (atmo_hit.x > atmo_hit.y) return vec3(0.0);
	vec2 ground_hit = sphere_intersect(origin, dir, u_planet_radius);
	float ray_start = max(atmo_hit.x, 0.0);
	float ray_end = ground_hit.x > 0.0 ? min(atmo_hit.y, ground_hit.x) : atmo_hit.y;
	if (ray_end <= ray_start) return vec3(0.0);
	float step_len = (ray_end - ray_start) / float(PRIMARY_STEPS);

	float mu = dot(dir, sun_dir);
	float phase_rlh = 3.0 / (16.0 * PI) * (1.0 + mu * mu);
	float phase_mie = mie_phase(mu, u_mie_g);

	vec3 total_rlh = vec3(0.0);
	vec3 total_mie = vec3(0.0);
	float od_rlh = 0.0;
	float od_mie = 0.0;
	float od_ozo = 0.0;
	float t = ray_start;
	float jitter = ray_jitter(dir);

	for (int i = 0; i < PRIMARY_STEPS; i++) {
		float sample_t = t + step_len * mix(0.32, 0.68, jitter);
		vec3 pos = origin + dir * sample_t;
		float height = max(length(pos) - u_planet_radius, 0.0);
		float step_rlh = exp(-height / u_rayleigh_scale_height) * step_len;
		float step_mie = exp(-height / u_mie_scale_height) * step_len;
		od_rlh += step_rlh;
		od_mie += step_mie;
		od_ozo += ozone_density(height) * step_len;

		float planet_vis = planet_direct_sun_visibility(pos, sun_dir, u_planet_radius);
		if (planet_vis > 0.001) {
			vec2 light_hit = sphere_intersect(pos, sun_dir, u_atmosphere_radius);
			float light_step_len = max(light_hit.y, 0.0) / float(LIGHT_STEPS);
			float light_od_rlh = 0.0;
			float light_od_mie = 0.0;
			float light_od_ozo = 0.0;
			float lt = 0.0;
			for (int j = 0; j < LIGHT_STEPS; j++) {
				vec3 light_pos = pos + sun_dir * (lt + light_step_len * 0.5);
				float light_height = max(length(light_pos) - u_planet_radius, 0.0);
				light_od_rlh += exp(-light_height / u_rayleigh_scale_height) * light_step_len;
				light_od_mie += exp(-light_height / u_mie_scale_height) * light_step_len;
				light_od_ozo += ozone_density(light_height) * light_step_len;
				lt += light_step_len;
			}

			vec3 attn = exp(-(u_mie_coeff * (od_mie + light_od_mie)
				+ u_rayleigh_coeff * (od_rlh + light_od_rlh)
				+ u_ozone_coeff * (od_ozo + light_od_ozo)));
			attn *= planet_vis;
			total_rlh += step_rlh * attn;
			total_mie += step_mie * attn;
		}
		t += step_len;
	}

	return u_sun_intensity * (phase_rlh * u_rayleigh_coeff * total_rlh
		+ phase_mie * u_mie_coeff * total_mie);
}

void sky() {
	vec3 up = normalize(u_up);
	vec3 origin = up * (u_planet_radius + max(u_camera_height, -500.0));
	vec3 dir = normalize(EYEDIR);
	vec3 sun_dir = normalize(LIGHT0_DIRECTION);
	vec3 col = scatter(dir, origin, sun_dir);
	if (LIGHT0_ENERGY > 0.0) {
		float cos_angle = dot(dir, sun_dir);
		// LIGHT0_SIZE is the angular diameter in radians. Use the real solar
		// diameter as a fallback and give the disc only a narrow antialiased edge.
		float sun_diameter = max(LIGHT0_SIZE, 0.0087);
		float sun_radius = sun_diameter * 0.5;
		float disc = smoothstep(cos(sun_radius), cos(sun_radius * 0.92), cos_angle);
		float visible = planet_direct_sun_visibility(origin, sun_dir, u_planet_radius);
		col += disc * visible * LIGHT0_COLOR * LIGHT0_ENERGY * 4.0;
	}
	COLOR = 1.0 - exp(-col);
}
