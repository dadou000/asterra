shader_type sky;
/// Physically-inspired single-scatter Rayleigh + Mie atmosphere.
///
/// A sky shader has no notion of where the camera actually sits on the
/// planet, so `u_up` / `u_camera_height` are pushed in from GDScript every
/// frame (see main.gd) instead of being derived here.

uniform vec3 u_up = vec3(0.0, 1.0, 0.0);
uniform float u_camera_height = 0.0;            // metres above mean surface
uniform float u_planet_radius = 1000000.0;
uniform float u_atmosphere_radius = 1060000.0;

uniform vec3 u_rayleigh_coeff = vec3(5.5e-6, 13.0e-6, 22.4e-6);
uniform float u_mie_coeff = 21e-6;
uniform float u_rayleigh_scale_height = 8000.0;
uniform float u_mie_scale_height = 1200.0;
uniform float u_mie_g = 0.758;
uniform float u_sun_intensity = 22.0;

const int PRIMARY_STEPS = 16;
const int LIGHT_STEPS = 8;

// Intersect a ray with a sphere centred on the origin. near > far means miss.
vec2 sphere_intersect(vec3 origin, vec3 dir, float radius) {
	float b = dot(dir, origin);
	float c = dot(origin, origin) - radius * radius;
	float disc = b * b - c;
	if (disc < 0.0) {
		return vec2(1e9, -1e9);
	}
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
}

vec3 scatter(vec3 dir, vec3 origin, vec3 sun_dir) {
	vec2 atmo_hit = sphere_intersect(origin, dir, u_atmosphere_radius);
	if (atmo_hit.x > atmo_hit.y) {
		return vec3(0.0);
	}
	vec2 ground_hit = sphere_intersect(origin, dir, u_planet_radius);
	float ray_start = max(atmo_hit.x, 0.0);
	float ray_end = ground_hit.x > 0.0 ? min(atmo_hit.y, ground_hit.x) : atmo_hit.y;
	if (ray_end <= ray_start) {
		return vec3(0.0);
	}
	float step_len = (ray_end - ray_start) / float(PRIMARY_STEPS);

	float mu = dot(dir, sun_dir);
	float mumu = mu * mu;
	float gg = u_mie_g * u_mie_g;
	float phase_rlh = 3.0 / (16.0 * PI) * (1.0 + mumu);
	float phase_mie = 3.0 / (8.0 * PI) * ((1.0 - gg) * (mumu + 1.0))
		/ (pow(1.0 + gg - 2.0 * mu * u_mie_g, 1.5) * (2.0 + gg));

	vec3 total_rlh = vec3(0.0);
	vec3 total_mie = vec3(0.0);
	float od_rlh = 0.0;
	float od_mie = 0.0;
	float t = ray_start;

	for (int i = 0; i < PRIMARY_STEPS; i++) {
		vec3 pos = origin + dir * (t + step_len * 0.5);
		float height = length(pos) - u_planet_radius;
		float step_rlh = exp(-height / u_rayleigh_scale_height) * step_len;
		float step_mie = exp(-height / u_mie_scale_height) * step_len;
		od_rlh += step_rlh;
		od_mie += step_mie;

		// Secondary ray toward the sun, to attenuate this sample by the
		// atmosphere it still has to cross to reach the light.
		vec2 light_hit = sphere_intersect(pos, sun_dir, u_atmosphere_radius);
		float light_step_len = light_hit.y / float(LIGHT_STEPS);
		float light_od_rlh = 0.0;
		float light_od_mie = 0.0;
		float lt = 0.0;
		bool occluded = false;
		for (int j = 0; j < LIGHT_STEPS; j++) {
			vec3 light_pos = pos + sun_dir * (lt + light_step_len * 0.5);
			float light_height = length(light_pos) - u_planet_radius;
			if (light_height < 0.0) {
				occluded = true;
				break;
			}
			light_od_rlh += exp(-light_height / u_rayleigh_scale_height) * light_step_len;
			light_od_mie += exp(-light_height / u_mie_scale_height) * light_step_len;
			lt += light_step_len;
		}

		if (!occluded) {
			vec3 attn = exp(-(u_mie_coeff * (od_mie + light_od_mie)
				+ u_rayleigh_coeff * (od_rlh + light_od_rlh)));
			total_rlh += step_rlh * attn;
			total_mie += step_mie * attn;
		}
		t += step_len;
	}

	return u_sun_intensity * (phase_rlh * u_rayleigh_coeff * total_rlh + phase_mie * u_mie_coeff * total_mie);
}

void sky() {
	vec3 up = normalize(u_up);
	vec3 origin = up * (u_planet_radius + max(u_camera_height, -500.0));
	vec3 dir = normalize(EYEDIR);
	vec3 sun_dir = normalize(LIGHT0_DIRECTION);

	vec3 col = scatter(dir, origin, sun_dir);

	// Visible sun disc, drawn on top of the scattered sky.
	if (LIGHT0_ENERGY > 0.0) {
		float cos_angle = dot(dir, sun_dir);
		float sun_radius = max(LIGHT0_SIZE, 0.02);
		float disc = smoothstep(cos(sun_radius), cos(sun_radius * 0.25), cos_angle);
		col += disc * LIGHT0_COLOR * LIGHT0_ENERGY * 4.0;
	}

	COLOR = 1.0 - exp(-col);
}
