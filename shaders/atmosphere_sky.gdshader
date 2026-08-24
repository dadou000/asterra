shader_type sky;
render_mode use_half_res_pass;
/// Physically-inspired single-scatter Rayleigh + Mie atmosphere plus a restrained
/// procedural deep-sky layer and spherical raymarched volumetric clouds.

uniform vec3 u_up = vec3(0.0, 1.0, 0.0);
uniform float u_camera_height = 0.0;
uniform float u_planet_radius = 1000000.0;
uniform float u_atmosphere_radius = 1060000.0;
uniform vec3 u_rayleigh_coeff = vec3(5.5e-6, 13.0e-6, 22.4e-6);
uniform float u_mie_coeff = 21e-6;
uniform float u_rayleigh_scale_height = 8000.0;
uniform float u_mie_scale_height = 1200.0;
uniform float u_mie_g = 0.72;
uniform vec3 u_ozone_coeff = vec3(0.650e-6, 1.881e-6, 0.085e-6);
uniform float u_ozone_centre = 25000.0;
uniform float u_ozone_width = 15000.0;
uniform float u_sun_intensity = 5.0265;

#include "res://shaders/planet_lighting.gdshaderinc"
#include "res://shaders/volumetric_clouds.gdshaderinc"
#include "res://shaders/cloud_raymarch_stable.gdshaderinc"

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

float mie_phase(float mu, float g) {
	float gg = g * g;
	return (3.0 * (1.0 - gg) * (1.0 + mu * mu))
		/ (8.0 * PI * (2.0 + gg) * pow(max(1.0 + gg - 2.0 * g * mu, 1e-4), 1.5));
}

float hash31(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.yzx + 33.33);
	return fract((p.x + p.y) * p.z);
}

float value_noise(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	float n000 = hash31(i + vec3(0.0, 0.0, 0.0));
	float n100 = hash31(i + vec3(1.0, 0.0, 0.0));
	float n010 = hash31(i + vec3(0.0, 1.0, 0.0));
	float n110 = hash31(i + vec3(1.0, 1.0, 0.0));
	float n001 = hash31(i + vec3(0.0, 0.0, 1.0));
	float n101 = hash31(i + vec3(1.0, 0.0, 1.0));
	float n011 = hash31(i + vec3(0.0, 1.0, 1.0));
	float n111 = hash31(i + vec3(1.0, 1.0, 1.0));
	return mix(
		mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
		mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y),
		f.z
	);
}

float fbm(vec3 p) {
	float v = 0.0;
	float a = 0.5;
	for (int i = 0; i < 4; i++) {
		v += value_noise(p) * a;
		p = p * 2.03 + vec3(11.3, 7.1, 5.7);
		a *= 0.5;
	}
	return v;
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
	float sample_phase = stable_primary_phase(dir);

	for (int i = 0; i < PRIMARY_STEPS; i++) {
		float sample_t = t + step_len * sample_phase;
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

vec3 deep_sky(vec3 dir, vec3 up) {
	// Fixed celestial frame: this is deliberately not aligned to local up, so the
	// player sees a coherent galaxy as they travel around the spherical planet.
	vec3 galactic_normal = normalize(vec3(0.28, 0.91, -0.30));
	vec3 galactic_axis = normalize(vec3(-0.84, 0.15, -0.52));
	float lat = dot(dir, galactic_normal);
	float lon = dot(dir, galactic_axis);
	float warp = (fbm(dir * 3.2 + vec3(4.7, 1.1, 8.3)) - 0.5) * 0.055;
	float band = exp(-pow(abs(lat + warp) / 0.105, 1.55));
	float broad = exp(-pow(abs(lat) / 0.27, 1.65)) * 0.30;
	float structure = mix(0.60, 1.22, fbm(dir * 8.5 + vec3(2.4, 7.0, 1.8)));
	float dust_noise = fbm(dir * 18.0 + vec3(8.0, 2.0, 5.0));
	float dust_lane = exp(-pow(abs(lat + 0.018 * sin(lon * 11.0)) / 0.030, 1.35));
	float dust = mix(1.0, 0.32, dust_lane * smoothstep(0.35, 0.72, dust_noise));
	float core = exp(-pow(length(vec2(lat * 2.8, lon - 0.18)) / 0.48, 1.35));

	// Fixed scene-linear radiances. They are never multiplied upward as night falls.
	vec3 galaxy = vec3(0.76, 0.80, 0.86) * (band * structure * dust * 0.0058);
	galaxy += vec3(0.92, 0.83, 0.68) * core * band * dust * 0.0030;
	galaxy += vec3(0.58, 0.66, 0.78) * broad * 0.0016;

	vec3 n1 = normalize(vec3(0.48, 0.22, -0.85));
	vec3 n2 = normalize(vec3(-0.62, 0.72, 0.31));
	vec3 n3 = normalize(vec3(0.14, -0.36, 0.92));
	float d1 = acos(clamp(dot(dir, n1), -1.0, 1.0));
	float d2 = acos(clamp(dot(dir, n2), -1.0, 1.0));
	float d3 = acos(clamp(dot(dir, n3), -1.0, 1.0));
	float neb_noise = mix(0.55, 1.0, fbm(dir * 22.0 + vec3(3.1, 9.0, 6.2)));
	vec3 nebula = vec3(0.42, 0.18, 0.17) * exp(-pow(d1 / 0.075, 1.7)) * 0.0014;
	nebula += vec3(0.15, 0.24, 0.34) * exp(-pow(d2 / 0.095, 1.8)) * 0.0012;
	nebula += vec3(0.30, 0.16, 0.27) * exp(-pow(d3 / 0.060, 1.6)) * 0.0010;
	nebula *= neb_noise;

	float atmosphere_height = max(u_atmosphere_radius - u_planet_radius, 1.0);
	float atmosphere_presence = 1.0 - smoothstep(
		atmosphere_height * 0.72,
		atmosphere_height * 1.08,
		u_camera_height
	);

	// Only physical line-of-sight loss is applied. There is deliberately no
	// sun-altitude/night reveal term: daylight invisibility emerges because the
	// same fixed deep-sky radiance is overwhelmed by atmospheric scattering and
	// the existing exposure system.
	float elevation = dot(dir, up);
	float horizon = smoothstep(-0.02, 0.12, elevation);
	float air_mass = 1.0 / max(elevation, 0.07);
	float extinction = exp(-0.085 * max(air_mass - 1.0, 0.0));
	float atmospheric_transmittance = mix(
		1.0,
		horizon * extinction,
		atmosphere_presence
	);
	return (galaxy + nebula) * atmospheric_transmittance;
}

void sky() {
	vec3 up = normalize(u_up);
	vec3 origin = up * (u_planet_radius + max(u_camera_height, -500.0));
	vec3 dir = normalize(EYEDIR);
	vec3 sun_dir = normalize(LIGHT0_DIRECTION);

	if (AT_HALF_RES_PASS) {
		vec3 sun_color = LIGHT0_ENABLED ? LIGHT0_COLOR : vec3(1.0);
		vec4 cloud = raymarch_clouds_stable(
			origin,
			dir,
			sun_dir,
			sun_color,
			u_sun_intensity,
			u_cloud_primary_steps,
			u_cloud_light_steps
		);
		COLOR = cloud.rgb;
		// The half-res alpha stores remaining background transmittance, not cloud
		// opacity. The full pass uses it to compose the background behind clouds.
		ALPHA = cloud.a;
	} else {
		vec3 col = scatter(dir, origin, sun_dir);
		col += deep_sky(dir, up);
		if (LIGHT0_ENABLED && LIGHT0_ENERGY > 0.0) {
			float cos_angle = dot(dir, sun_dir);
			float sun_diameter = max(LIGHT0_SIZE, 0.0087);
			float sun_radius = sun_diameter * 0.5;
			float disc = smoothstep(cos(sun_radius), cos(sun_radius * 0.92), cos_angle);
			float visible = planet_direct_sun_visibility(origin, sun_dir, u_planet_radius);
			col += disc * visible * LIGHT0_COLOR * LIGHT0_ENERGY * 4.0;
		}

		vec4 cloud = HALF_RES_COLOR;
		col = cloud.rgb + col * cloud.a;
		COLOR = 1.0 - exp(-col);
		ALPHA = 1.0;
	}
}