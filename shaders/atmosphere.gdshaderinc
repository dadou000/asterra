/// Aerial perspective: the air standing between the camera and a surface.
///
/// atmosphere_sky.gdshader integrates the same single-scatter Rayleigh + Mie
/// model along a ray that never stops at anything, which is only correct for the
/// background. Geometry needs the other half: over a hundred kilometres of air
/// the ground is extinguished and replaced by scattered sunlight, and without
/// that the atmosphere reads as a backdrop pinned behind the planet rather than
/// something the planet sits inside.
///
/// The coefficients, scale heights, phase functions and the final `1 - exp(-x)`
/// compression are deliberately identical to the sky's, so the two agree either
/// side of a silhouette. They differ only in where the ray stops -- which is the
/// whole point.

uniform float u_planet_radius = 1000000.0;
uniform float u_atmosphere_height = 60000.0;
/// Sunward, planet frame.
uniform vec3 u_sun_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_rayleigh_coeff = vec3(5.5e-6, 13.0e-6, 22.4e-6);
uniform float u_mie_coeff = 21e-6;
uniform float u_rayleigh_scale_height = 8000.0;
uniform float u_mie_scale_height = 1200.0;
uniform float u_mie_g = 0.758;
uniform float u_sun_intensity = 22.0;
/// The sky ends on `1 - exp(-radiance)`, a curve that pulls a full daylight path
/// down by rather more than half. A short segment sits near the straight part of
/// that same curve and so would keep almost all of its in-scatter -- five
/// kilometres of clear air would then contribute a fifth of the brightness of
/// the whole sky. This is the average slope of the sky's curve over daylight
/// radiances, applied before the compression so that near and far end up on one
/// scale. Turn it down for thinner air, up for a hazier world.
uniform float u_aerial_strength : hint_range(0.0, 2.0) = 0.45;

#include "res://shaders/cloud_shadow.gdshaderinc"

// The original 8x3 integration was needlessly expensive on every terrain and
// water pixel. Five camera samples and two sun-path samples retain smooth aerial
// perspective while leaving budget for weather shadowing.
const int AP_STEPS = 5;
const int AP_LIGHT_STEPS = 2;

vec2 atmo_sphere_hit(vec3 origin, vec3 dir, float radius) {
	float b = dot(dir, origin);
	float c = dot(origin, origin) - radius * radius;
	float disc = b * b - c;
	if (disc < 0.0) {
		return vec2(1e9, -1e9);
	}
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
}

/// FOG for the segment `cam` -> `frag`, both planet-frame. Returned as Godot's
/// fog pair: the colour to blend towards, and how far to blend. The colour is
/// pre-divided by the amount so that a thin segment adds its in-scatter without
/// also washing the surface out.
vec4 aerial_fog(vec3 cam, vec3 frag) {
	vec3 delta = frag - cam;
	float seg = length(delta);
	if (seg < 25.0) {
		return vec4(0.0);
	}
	vec3 dir = delta / seg;
	float top = u_planet_radius + u_atmosphere_height;
	// Only the part of the segment actually inside the atmosphere counts; from
	// orbit that is a thin shell at the far end of a very long ray.
	vec2 hit = atmo_sphere_hit(cam, dir, top);
	float t0 = max(hit.x, 0.0);
	float t1 = min(hit.y, seg);
	if (t1 <= t0) {
		return vec4(0.0);
	}

	float step_len = (t1 - t0) / float(AP_STEPS);
	float od_rlh = 0.0;
	float od_mie = 0.0;
	vec3 total_rlh = vec3(0.0);
	vec3 total_mie = vec3(0.0);
	float t = t0;
	for (int i = 0; i < AP_STEPS; i++) {
		vec3 pos = cam + dir * (t + step_len * 0.5);
		float height = length(pos) - u_planet_radius;
		float step_rlh = exp(-height / u_rayleigh_scale_height) * step_len;
		float step_mie = exp(-height / u_mie_scale_height) * step_len;
		od_rlh += step_rlh;
		od_mie += step_mie;

		// Secondary ray to the sun: air in the planet's own shadow scatters
		// nothing. The weather mask additionally blocks sunlight underneath cloud
		// decks, so alternating lit/shaded columns become crepuscular rays.
		vec2 light_hit = atmo_sphere_hit(pos, u_sun_dir, top);
		float light_step = light_hit.y / float(AP_LIGHT_STEPS);
		float light_od_rlh = 0.0;
		float light_od_mie = 0.0;
		float lt = 0.0;
		bool occluded = false;
		for (int j = 0; j < AP_LIGHT_STEPS; j++) {
			vec3 light_pos = pos + u_sun_dir * (lt + light_step * 0.5);
			float light_height = length(light_pos) - u_planet_radius;
			if (light_height < 0.0) {
				occluded = true;
				break;
			}
			light_od_rlh += exp(-light_height / u_rayleigh_scale_height) * light_step;
			light_od_mie += exp(-light_height / u_mie_scale_height) * light_step;
			lt += light_step;
		}
		if (!occluded) {
			vec3 attn = exp(-(u_mie_coeff * (od_mie + light_od_mie)
				+ u_rayleigh_coeff * (od_rlh + light_od_rlh)));
			attn *= cloud_sun_visibility(pos, u_sun_dir);
			total_rlh += step_rlh * attn;
			total_mie += step_mie * attn;
		}
		t += step_len;
	}

	float mu = dot(dir, u_sun_dir);
	float mumu = mu * mu;
	float gg = u_mie_g * u_mie_g;
	float phase_rlh = 3.0 / (16.0 * PI) * (1.0 + mumu);
	float phase_mie = 3.0 / (8.0 * PI) * ((1.0 - gg) * (mumu + 1.0))
		/ (pow(max(1.0 + gg - 2.0 * mu * u_mie_g, 1e-4), 1.5) * (2.0 + gg));
	vec3 inscatter = u_sun_intensity
		* (phase_rlh * u_rayleigh_coeff * total_rlh + phase_mie * u_mie_coeff * total_mie);

	// How much of the surface survives the trip. Godot's fog blend is a single
	// scalar, so the three channels have to be averaged here -- the colour of the
	// haze still comes from the in-scatter, only the depth of it is shared.
	vec3 extinction = exp(-(u_rayleigh_coeff * od_rlh + u_mie_coeff * od_mie));
	float amount = clamp(1.0 - dot(extinction, vec3(0.33333)), 0.0, 1.0);
	if (amount < 5e-4) {
		return vec4(0.0);
	}
	return vec4((1.0 - exp(-inscatter * u_aerial_strength)) / amount, amount);
}
