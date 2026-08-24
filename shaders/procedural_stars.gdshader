shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never, blend_add, fog_disabled;

// GPU-instanced unresolved stellar points. Stellar radiance is fixed by apparent
// magnitude and is NEVER raised when the sky gets dark. Day/night visibility must
// emerge from real competition with atmospheric radiance, exposure and tonemapping.
uniform vec3 u_up = vec3(0.0, 1.0, 0.0);
uniform float u_camera_height = 0.0;
uniform float u_atmosphere_height = 60000.0;

// Debug/validation overrides. Defaults are the physically calibrated path and are
// constant through the day/night cycle; no automatic visibility compensation uses them.
uniform float u_star_radiance_scale : hint_range(0.0, 2.0) = 1.0;
uniform float u_magnitude_flux_exponent : hint_range(0.5, 1.5) = 1.0;
uniform float u_seeing_strength : hint_range(0.0, 2.0) = 1.0;
uniform float u_colour_strength : hint_range(0.0, 1.0) = 1.0;
uniform float u_chromatic_scintillation_strength : hint_range(0.0, 2.0) = 1.0;

const float BRIGHTEST_MAGNITUDE = -1.5;
const float NAKED_EYE_LIMIT_MAGNITUDE = 6.5;
const float MIN_STELLAR_TEMPERATURE_K = 2400.0;
const float MAX_STELLAR_TEMPERATURE_K = 18000.0;
// Fixed scene-linear calibration. At physical magnitude response (1.0), the
// -1.5 to +6.5 catalogue spans about 1585:1 in stellar flux.
const float ZERO_MAGNITUDE_RADIANCE = 0.36;
const vec3 REC709_LUMA = vec3(0.2126, 0.7152, 0.0722);

varying vec3 v_star_dir;
varying float v_magnitude_norm;
varying float v_temperature_norm;
varying float v_pattern;
varying float v_phase;

vec3 srgb_to_linear(vec3 c) {
	vec3 low = c / 12.92;
	vec3 high = pow((c + vec3(0.055)) / 1.055, vec3(2.4));
	return mix(low, high, step(vec3(0.04045), c));
}

vec3 normalize_luminance(vec3 colour, float target_luminance) {
	float current = max(dot(colour, REC709_LUMA), 1e-5);
	return colour * (target_luminance / current);
}

vec3 blackbody_colour(float temperature_norm, float magnitude_norm) {
	float kelvin = mix(MIN_STELLAR_TEMPERATURE_K, MAX_STELLAR_TEMPERATURE_K, clamp(temperature_norm, 0.0, 1.0));
	float t = kelvin / 100.0;
	float r;
	float g;
	float b;
	if (t <= 66.0) {
		r = 1.0;
		g = clamp(0.3900815788 * log(max(t, 1.0)) - 0.6318414438, 0.0, 1.0);
		b = t <= 19.0 ? 0.0 : clamp(0.5432067891 * log(max(t - 10.0, 1.0)) - 1.1962540891, 0.0, 1.0);
	} else {
		float hot_t = max(t - 60.0, 1.0);
		r = clamp(1.2929361861 * pow(hot_t, -0.1332047592), 0.0, 1.0);
		g = clamp(1.1298908610 * pow(hot_t, -0.0755148492), 0.0, 1.0);
		b = 1.0;
	}
	vec3 colour = normalize_luminance(srgb_to_linear(vec3(r, g, b)), 1.0);
	float magnitude = mix(BRIGHTEST_MAGNITUDE, NAKED_EYE_LIMIT_MAGNITUDE, clamp(magnitude_norm, 0.0, 1.0));
	float cone_response = 1.0 - smoothstep(0.5, 5.8, magnitude);
	float visible_chroma = mix(0.18, 0.90, cone_response) * clamp(u_colour_strength, 0.0, 1.0);
	colour = mix(vec3(1.0), colour, visible_chroma);
	return normalize_luminance(colour, 1.0);
}

float stellar_radiance(float magnitude_norm) {
	float magnitude = mix(BRIGHTEST_MAGNITUDE, NAKED_EYE_LIMIT_MAGNITUDE, clamp(magnitude_norm, 0.0, 1.0));
	float response = clamp(u_magnitude_flux_exponent, 0.5, 1.5);
	return ZERO_MAGNITUDE_RADIANCE * clamp(u_star_radiance_scale, 0.0, 2.0) * pow(10.0, -0.4 * magnitude * response);
}

float hash11(float p) {
	return fract(sin(p * 127.1 + 311.7) * 43758.5453123);
}

float temporal_noise(float x, float seed) {
	float i = floor(x);
	float f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	float a = hash11(i + seed * 19.19);
	float b = hash11(i + 1.0 + seed * 19.19);
	return mix(a, b, f) * 2.0 - 1.0;
}

float scintillation_signal(float kind, float t, float seed) {
	if (kind < 0.5) return 0.0;
	if (kind < 1.5) {
		float n1 = temporal_noise(t * 2.7, seed + 1.7);
		float n2 = temporal_noise(t * 6.1, seed + 7.3);
		return clamp(n1 * 0.64 + n2 * 0.36, -1.0, 1.0);
	}
	if (kind < 2.5) {
		float n1 = temporal_noise(t * 4.1, seed + 2.1);
		float n2 = temporal_noise(t * 9.3, seed + 11.8);
		float n3 = temporal_noise(t * 15.7, seed + 23.4);
		return clamp(n1 * 0.48 + n2 * 0.34 + n3 * 0.18, -1.0, 1.0);
	}
	if (kind < 3.5) {
		float envelope = 0.42 + 0.58 * (temporal_noise(t * 0.62, seed + 31.2) * 0.5 + 0.5);
		float carrier = temporal_noise(t * 5.2, seed + 3.6) * 0.57 + temporal_noise(t * 12.6, seed + 17.1) * 0.43;
		return clamp(carrier * envelope, -1.0, 1.0);
	}
	float envelope = 0.25 + 0.75 * (temporal_noise(t * 0.91, seed + 41.7) * 0.5 + 0.5);
	float carrier = temporal_noise(t * 6.7, seed + 4.9) * 0.44 + temporal_noise(t * 14.8, seed + 19.3) * 0.34 + temporal_noise(t * 23.1, seed + 37.5) * 0.22;
	return clamp(carrier * envelope, -1.0, 1.0);
}

float pattern_gain(float kind) {
	if (kind < 0.5) return 0.0;
	if (kind < 1.5) return 0.32;
	if (kind < 2.5) return 0.52;
	if (kind < 3.5) return 0.76;
	return 1.0;
}

vec3 chromatic_scintillation(vec3 base_colour, float kind, float t, float seed, float elevation, float atmosphere_presence) {
	float low_altitude = 1.0 - smoothstep(0.035, 0.34, max(elevation, 0.0));
	float amount = atmosphere_presence * low_altitude * pattern_gain(kind) * 0.085 * clamp(u_chromatic_scintillation_strength, 0.0, 2.0);
	if (amount <= 0.0001) return base_colour;
	float rb = temporal_noise(t * 12.4, seed + 73.1);
	float green = temporal_noise(t * 8.9, seed + 91.7);
	vec3 spectral_shift = vec3(1.0 + amount * rb, 1.0 + amount * 0.24 * green, 1.0 - amount * rb);
	vec3 shifted = max(base_colour * spectral_shift, vec3(0.0));
	return normalize_luminance(shifted, dot(base_colour, REC709_LUMA));
}

void vertex() {
	vec3 camera_world = INV_VIEW_MATRIX[3].xyz;
	vec3 centre_world = (MODEL_MATRIX * vec4(vec3(0.0), 1.0)).xyz;
	v_star_dir = normalize(centre_world - camera_world);
	float sx = length(MODEL_MATRIX[0].xyz);
	float sy = length(MODEL_MATRIX[1].xyz);
	vec3 camera_right = normalize(INV_VIEW_MATRIX[0].xyz);
	vec3 camera_up = normalize(INV_VIEW_MATRIX[1].xyz);
	vec3 world_vertex = centre_world + camera_right * VERTEX.x * sx + camera_up * VERTEX.y * sy;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * vec4(world_vertex, 1.0);
	v_magnitude_norm = INSTANCE_CUSTOM.x;
	v_temperature_norm = INSTANCE_CUSTOM.y;
	v_pattern = INSTANCE_CUSTOM.z;
	v_phase = INSTANCE_CUSTOM.w;
}

void fragment() {
	vec2 p = UV * 2.0 - 1.0;
	float r2 = dot(p, p);
	float profile = exp(-r2 * 4.8) * (1.0 - smoothstep(0.72, 1.0, r2));
	if (profile <= 0.001) discard;
	vec3 up = normalize(u_up);
	float atmosphere_height = max(u_atmosphere_height, 1.0);
	float atmosphere_presence = 1.0 - smoothstep(atmosphere_height * 0.72, atmosphere_height * 1.08, u_camera_height);
	float elevation = dot(normalize(v_star_dir), up);
	float above_horizon = smoothstep(-0.012, 0.045, elevation);
	float air_mass = 1.0 / max(elevation, 0.055);
	float excess_air_mass = max(air_mass - 1.0, 0.0);
	vec3 extinction = exp(-vec3(0.045, 0.060, 0.092) * excess_air_mass);
	extinction = mix(vec3(1.0), extinction, atmosphere_presence);
	float horizon_visibility = mix(1.0, above_horizon, atmosphere_presence);
	float kind = floor(v_pattern * 4.0 + 0.5);
	float gain = pattern_gain(kind);
	float near_horizon = 1.0 - smoothstep(0.07, 0.82, max(elevation, 0.0));
	float turbulent_path = pow(near_horizon, 1.35);
	float base_amplitude = mix(0.012, 0.58, turbulent_path);
	float scintillation_amplitude = atmosphere_presence * clamp(u_seeing_strength, 0.0, 2.0) * gain * base_amplitude;
	float seed = v_phase * 4096.0 + dot(v_star_dir, vec3(17.0, 59.0, 113.0));
	float signal = scintillation_signal(kind, TIME, seed);
	float scintillation = max(0.0, 1.0 + scintillation_amplitude * signal);
	vec3 colour = blackbody_colour(v_temperature_norm, v_magnitude_norm);
	colour = chromatic_scintillation(colour, kind, TIME, seed, elevation, atmosphere_presence);
	float radiance = stellar_radiance(v_magnitude_norm);
	EMISSION = colour * extinction * radiance * scintillation;
	ALPHA = profile * horizon_visibility;
}
