/// GPU-only aerial perspective for the spherical terrain clipmap.
///
/// This intentionally evaluates on the terrain vertex lattice and interpolates
/// the result across fragments. Atmospheric extinction/in-scattering varies on
/// kilometre scales, so doing the integration per pixel would waste a large
/// amount of GPU time at 1440p/4K without adding visible detail.
///
/// No CPU terrain work, streaming, readback, or per-tile generation is involved.

uniform float u_terrain_atmosphere_height = 60000.0;
uniform float u_terrain_aerial_strength : hint_range(0.0, 2.0) = 0.62;

const vec3 TERRAIN_RAYLEIGH_COEFF = vec3(5.5e-6, 13.0e-6, 22.4e-6);
const float TERRAIN_MIE_COEFF = 21e-6;
const vec3 TERRAIN_OZONE_COEFF = vec3(0.650e-6, 1.881e-6, 0.085e-6);
const float TERRAIN_RAYLEIGH_SCALE_HEIGHT = 8000.0;
const float TERRAIN_MIE_SCALE_HEIGHT = 1200.0;
const float TERRAIN_MIE_G = 0.758;
const float TERRAIN_OZONE_CENTRE = 25000.0;
const float TERRAIN_OZONE_WIDTH = 15000.0;
const float TERRAIN_SOLAR_IRRADIANCE = 5.0265482457; // 1.6 * PI

// Three view samples and one light-column sample are sufficient here because
// the result is vertex-interpolated. The sky shader remains the higher-quality
// reference integration.
const int TERRAIN_AP_STEPS = 3;
const int TERRAIN_AP_LIGHT_STEPS = 1;

float terrain_atmo_ozone_density(float height) {
	return max(1.0 - abs(height - TERRAIN_OZONE_CENTRE) / TERRAIN_OZONE_WIDTH, 0.0);
}

float terrain_atmo_mie_phase(float mu, float g) {
	float gg = g * g;
	return (3.0 * (1.0 - gg) * (1.0 + mu * mu))
		/ (8.0 * PI * (2.0 + gg) * pow(max(1.0 + gg - 2.0 * g * mu, 1e-4), 1.5));
}

vec2 terrain_atmo_sphere_hit(vec3 origin, vec3 dir, float radius) {
	float b = dot(dir, origin);
	float c = dot(origin, origin) - radius * radius;
	float disc = b * b - c;
	if (disc < 0.0) return vec2(1e9, -1e9);
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
}

void terrain_aerial_perspective(vec3 cam, vec3 frag, vec3 sun_direction,
		float planet_radius, out vec3 transmittance, out vec3 inscatter) {
	transmittance = vec3(1.0);
	inscatter = vec3(0.0);

	vec3 delta = frag - cam;
	float segment_length = length(delta);
	// The near field does not need an atmospheric integration. Apart from being
	// cheaper, this avoids tiny numerical colour shifts while walking on ground.
	if (segment_length < 80.0) return;

	vec3 dir = delta / segment_length;
	float atmosphere_radius = planet_radius + u_terrain_atmosphere_height;
	vec2 hit = terrain_atmo_sphere_hit(cam, dir, atmosphere_radius);
	float t0 = max(hit.x, 0.0);
	float t1 = min(hit.y, segment_length);
	if (t1 <= t0) return;

	float step_length = (t1 - t0) / float(TERRAIN_AP_STEPS);
	float od_rayleigh = 0.0;
	float od_mie = 0.0;
	float od_ozone = 0.0;
	vec3 total_rayleigh = vec3(0.0);
	vec3 total_mie = vec3(0.0);
	vec3 sun_dir = normalize(sun_direction);

	for (int i = 0; i < TERRAIN_AP_STEPS; i++) {
		float sample_t = t0 + (float(i) + 0.5) * step_length;
		vec3 pos = cam + dir * sample_t;
		float height = max(length(pos) - planet_radius, 0.0);
		float step_rayleigh = exp(-height / TERRAIN_RAYLEIGH_SCALE_HEIGHT) * step_length;
		float step_mie = exp(-height / TERRAIN_MIE_SCALE_HEIGHT) * step_length;
		od_rayleigh += step_rayleigh;
		od_mie += step_mie;
		od_ozone += terrain_atmo_ozone_density(height) * step_length;

		float planet_vis = planet_direct_sun_visibility(pos, sun_dir, planet_radius);
		if (planet_vis > 0.001) {
			vec2 light_hit = terrain_atmo_sphere_hit(pos, sun_dir, atmosphere_radius);
			float light_length = max(light_hit.y, 0.0);
			float light_step = light_length / float(TERRAIN_AP_LIGHT_STEPS);
			float light_od_rayleigh = 0.0;
			float light_od_mie = 0.0;
			float light_od_ozone = 0.0;

			for (int j = 0; j < TERRAIN_AP_LIGHT_STEPS; j++) {
				vec3 light_pos = pos + sun_dir * ((float(j) + 0.5) * light_step);
				float light_height = max(length(light_pos) - planet_radius, 0.0);
				light_od_rayleigh += exp(-light_height / TERRAIN_RAYLEIGH_SCALE_HEIGHT) * light_step;
				light_od_mie += exp(-light_height / TERRAIN_MIE_SCALE_HEIGHT) * light_step;
				light_od_ozone += terrain_atmo_ozone_density(light_height) * light_step;
			}

			vec3 attenuation = exp(-(
				TERRAIN_MIE_COEFF * (od_mie + light_od_mie)
				+ TERRAIN_RAYLEIGH_COEFF * (od_rayleigh + light_od_rayleigh)
				+ TERRAIN_OZONE_COEFF * (od_ozone + light_od_ozone)));
			attenuation *= planet_vis;
			total_rayleigh += step_rayleigh * attenuation;
			total_mie += step_mie * attenuation;
		}
	}

	float mu = dot(dir, sun_dir);
	float phase_rayleigh = 3.0 / (16.0 * PI) * (1.0 + mu * mu);
	float phase_mie = terrain_atmo_mie_phase(mu, TERRAIN_MIE_G);
	inscatter = TERRAIN_SOLAR_IRRADIANCE * u_terrain_aerial_strength
		* (phase_rayleigh * TERRAIN_RAYLEIGH_COEFF * total_rayleigh
			+ phase_mie * TERRAIN_MIE_COEFF * total_mie);
	transmittance = exp(-(
		TERRAIN_RAYLEIGH_COEFF * od_rayleigh
		+ TERRAIN_MIE_COEFF * od_mie
		+ TERRAIN_OZONE_COEFF * od_ozone));
}
