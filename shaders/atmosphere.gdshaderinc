/// Aerial perspective: the air standing between the camera and a surface.
///
/// Geometry uses a cheap finite-segment approximation.

uniform float u_planet_radius = 1000000.0;
uniform float u_atmosphere_height = 60000.0;
uniform vec3 u_sun_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_rayleigh_coeff = vec3(5.5e-6, 13.0e-6, 22.4e-6);
uniform float u_mie_coeff = 21e-6;
uniform float u_rayleigh_scale_height = 8000.0;
uniform float u_mie_scale_height = 1200.0;
uniform float u_mie_g = 0.758;
/// Ozone. It scatters nothing and absorbs in the Chappuis band, taking a bite
/// out of the middle of the spectrum along long slant paths. That single term is
/// why a real twilight limb stays deep blue instead of drifting to grey-brown,
/// and it only shows where the path is long -- which is exactly the limb.
uniform vec3 u_ozone_coeff = vec3(0.650e-6, 1.881e-6, 0.085e-6);
uniform float u_ozone_centre = 25000.0;
uniform float u_ozone_width = 15000.0;
/// Top-of-atmosphere solar irradiance, in the units the surface is lit in.
/// Must equal the sun's Godot light energy times PI -- see Main.solar_irradiance,
/// which sets it. A value that disagrees with the sun does not read as a bright
/// sky, it reads as a pale wash over every distant surface.
uniform float u_sun_intensity = 5.0265;
/// Dial on the in-scattering, and the one deliberately unphysical number here.
///
/// At 1.0 this is exactly what the single-scattering integral produced, and that
/// is genuinely what a sensor at the top of the atmosphere measures: over dark
/// vegetation, in the blue, the path radiance really does exceed the light
/// coming off the ground, which is why raw satellite imagery is a blue haze and
/// why atmospheric correction is the first step of processing it.
///
/// A person looking down does not see that, because vision discounts a uniform
/// veil -- and nothing downstream here models an observer adapting to one. The
/// exposure controller works on frame average, so a bright veil actually makes
/// it stop down and crush the surface further, costing the land twice. Until
/// there is a real adaptation model, this dial stands in for it. The shape of
/// the term -- its spectrum, and how it grows with path length -- is untouched
/// and physical; only its overall weight is pulled back.
uniform float u_aerial_strength : hint_range(0.0, 2.0) = 0.62;

#include "res://shaders/planet_lighting.gdshaderinc"

const int AP_STEPS = 4;
const int AP_LIGHT_STEPS = 2;

/// Ozone is a layer, not an exponential: a tent peaked in the stratosphere.
float atmo_ozone_density(float height) {
	return max(1.0 - abs(height - u_ozone_centre) / u_ozone_width, 0.0);
}

/// Cornette-Shanks. Better behaved than raw Henyey-Greenstein at the forward
/// peak, so aerosol haze keeps its sun-direction dependence without collapsing
/// into a blown-out flare around the disc.
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

/// Aerial perspective between the camera and a surface, as the two things it
/// physically is: a per-channel transmittance and an added in-scattered
/// radiance.
///
/// This used to be packed into Godot's `FOG`, which is a colour and a single
/// scalar blend. That works for a grey fog and is wrong for an atmosphere: the
/// whole point of Rayleigh scattering is that it removes blue four times as fast
/// as red, and a scalar blend cannot express that. Collapsing the transmittance
/// to its own mean took the same bite out of every channel, which is precisely
/// how a landscape loses its colour with distance instead of going blue -- warm
/// rock and dark forest both drifted to the same grey rather than separating.
/// The caller multiplies its own outgoing radiance by `transmittance` and adds
/// `inscatter`, which is the same composition Godot would have done, only with
/// the spectrum kept.
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

		float planet_vis = planet_direct_sun_visibility(
			pos, sun_dir, u_planet_radius);
		if (planet_vis > 0.001) {
			vec2 light_hit = atmo_sphere_hit(pos, sun_dir, top);
			float light_len = max(light_hit.y, 0.0);
			float light_step = light_len / float(AP_LIGHT_STEPS);
			float light_od_rlh = 0.0;
			float light_od_mie = 0.0;
			float light_od_ozo = 0.0;
			float lt = 0.0;

			for (int j = 0; j < AP_LIGHT_STEPS; j++) {
				vec3 light_pos = pos + sun_dir * (lt + light_step * 0.5);
				float light_height = max(
					length(light_pos) - u_planet_radius, 0.0);
				light_od_rlh += exp(-light_height / u_rayleigh_scale_height)
					* light_step;
				light_od_mie += exp(-light_height / u_mie_scale_height)
					* light_step;
				light_od_ozo += atmo_ozone_density(light_height) * light_step;
				lt += light_step;
			}

			vec3 attn = exp(-(u_mie_coeff * (od_mie + light_od_mie)
				+ u_rayleigh_coeff * (od_rlh + light_od_rlh)
				+ u_ozone_coeff * (od_ozo + light_od_ozo)));
			// Only astronomical planet occultation belongs in this finite
			// foreground fog approximation.
			attn *= planet_vis;
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
