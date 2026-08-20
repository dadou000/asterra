/// Aerial perspective: the air standing between the camera and a surface.
///
/// Geometry uses a cheap finite-segment approximation. Cloud *surface* shadows
/// and the visible cloud volume are handled independently; this path must not use
/// one cloud sample to tint an entire camera-to-surface segment, because that
/// creates kilometre-scale blocks when the sample crosses a cloud-mask edge.

uniform float u_planet_radius = 1000000.0;
uniform float u_atmosphere_height = 60000.0;
uniform vec3 u_sun_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_rayleigh_coeff = vec3(5.5e-6, 13.0e-6, 22.4e-6);
uniform float u_mie_coeff = 21e-6;
uniform float u_rayleigh_scale_height = 8000.0;
uniform float u_mie_scale_height = 1200.0;
uniform float u_mie_g = 0.758;
uniform float u_sun_intensity = 22.0;
uniform float u_aerial_strength : hint_range(0.0, 2.0) = 0.45;

#include "res://shaders/planet_lighting.gdshaderinc"
#include "res://shaders/cloud_shadow.gdshaderinc"

const int AP_STEPS = 4;
const int AP_LIGHT_STEPS = 2;

vec2 atmo_sphere_hit(vec3 origin, vec3 dir, float radius) {
	float b = dot(dir, origin);
	float c = dot(origin, origin) - radius * radius;
	float disc = b * b - c;
	if (disc < 0.0) return vec2(1e9, -1e9);
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
}

vec4 aerial_fog(vec3 cam, vec3 frag) {
	vec3 delta = frag - cam;
	float seg = length(delta);
	if (seg < 25.0) return vec4(0.0);
	vec3 dir = delta / seg;
	float top = u_planet_radius + u_atmosphere_height;
	vec2 hit = atmo_sphere_hit(cam, dir, top);
	float t0 = max(hit.x, 0.0);
	float t1 = min(hit.y, seg);
	if (t1 <= t0) return vec4(0.0);

	float step_len = (t1 - t0) / float(AP_STEPS);
	float od_rlh = 0.0;
	float od_mie = 0.0;
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

		float planet_vis = planet_direct_sun_visibility(
			pos, sun_dir, u_planet_radius);
		if (planet_vis > 0.001) {
			vec2 light_hit = atmo_sphere_hit(pos, sun_dir, top);
			float light_len = max(light_hit.y, 0.0);
			float light_step = light_len / float(AP_LIGHT_STEPS);
			float light_od_rlh = 0.0;
			float light_od_mie = 0.0;
			float lt = 0.0;

			for (int j = 0; j < AP_LIGHT_STEPS; j++) {
				vec3 light_pos = pos + sun_dir * (lt + light_step * 0.5);
				float light_height = max(
					length(light_pos) - u_planet_radius, 0.0);
				light_od_rlh += exp(-light_height / u_rayleigh_scale_height)
					* light_step;
				light_od_mie += exp(-light_height / u_mie_scale_height)
					* light_step;
				lt += light_step;
			}

			vec3 attn = exp(-(u_mie_coeff * (od_mie + light_od_mie)
				+ u_rayleigh_coeff * (od_rlh + light_od_rlh)));
			// Only astronomical planet occultation belongs in this finite
			// foreground fog approximation. Cloud shadowing is evaluated at the
			// actual surface and inside the visible cloud volume; the sky shader
			// separately handles broad crepuscular modulation.
			attn *= planet_vis;
			total_rlh += step_rlh * attn;
			total_mie += step_mie * attn;
		}
		t += step_len;
	}

	float mu = dot(dir, sun_dir);
	float mumu = mu * mu;
	float gg = u_mie_g * u_mie_g;
	float phase_rlh = 3.0 / (16.0 * PI) * (1.0 + mumu);
	float phase_mie = 3.0 / (8.0 * PI)
		* ((1.0 - gg) * (mumu + 1.0))
		/ (pow(max(1.0 + gg - 2.0 * mu * u_mie_g, 1e-4), 1.5)
			* (2.0 + gg));
	vec3 inscatter = u_sun_intensity
		* (phase_rlh * u_rayleigh_coeff * total_rlh
			+ phase_mie * u_mie_coeff * total_mie);

	vec3 extinction = exp(
		-(u_rayleigh_coeff * od_rlh + u_mie_coeff * od_mie));
	float amount = clamp(
		1.0 - dot(extinction, vec3(0.33333)), 0.0, 1.0);
	if (amount < 5e-4) return vec4(0.0);
	return vec4(
		(1.0 - exp(-inscatter * u_aerial_strength)) / amount,
		amount);
}
