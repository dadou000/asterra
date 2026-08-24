shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never, blend_add, fog_disabled;

// GPU-instanced unresolved stellar points. Stellar radiance is fixed by apparent
// magnitude and is NEVER raised when the sky gets dark. Day/night visibility must
// emerge from real competition with atmospheric radiance, exposure and tonemapping.
uniform vec3 u_up = vec3(0.0, 1.0, 0.0);
uniform float u_camera_height = 0.0;
uniform float u_atmosphere_height = 60000.0;

const float BRIGHTEST_MAGNITUDE = -1.5;
const float NAKED_EYE_LIMIT_MAGNITUDE = 6.5;
// One fixed scene-linear photometric calibration for every time of day. It is
// never modified to make the stars prettier or more visible.
const float ZERO_MAGNITUDE_RADIANCE = 0.36;

varying vec3 v_star_dir;
varying float v_magnitude_norm;
varying float v_temperature;
varying float v_pattern;
varying float v_phase;

vec3 stellar_colour(float t) {
	vec3 warm = vec3(1.00, 0.79, 0.66);
	vec3 neutral = vec3(1.00, 0.96, 0.91);
	vec3 cool = vec3(0.78, 0.87, 1.00);
	if (t < 0.48) {
		return mix(warm, neutral, smoothstep(0.0, 0.48, t));
	}
	return mix(neutral, cool, smoothstep(0.48, 1.0, t));
}

float stellar_radiance(float magnitude_norm) {
	float magnitude = mix(
		BRIGHTEST_MAGNITUDE,
		NAKED_EYE_LIMIT_MAGNITUDE,
		clamp(magnitude_norm, 0.0, 1.0)
	);
	// Pogson magnitude law: five magnitudes = exactly 100x in flux.
	return ZERO_MAGNITUDE_RADIANCE * pow(10.0, -0.4 * magnitude);
}

float twinkle_pattern(float kind, float t, float phase) {
	// Scintillation is the only time-varying stellar flux. Every pattern is
	// centred on 1.0 so twinkle does not secretly increase average brightness.
	if (kind < 0.5) return 1.0;
	if (kind < 1.5) return 1.0 + 0.035 * sin(t * 5.4 + phase);
	if (kind < 2.5) {
		float a = sin(t * 3.1 + phase);
		float b = sin(t * 8.7 + phase * 2.17);
		return 1.0 + 0.022 * a + 0.014 * b;
	}
	if (kind < 3.5) {
		float s = sin(t * 2.2 + phase);
		return 1.0 + 0.040 * s * s * s;
	}
	float irregular = sin(t * 4.1 + phase)
		+ 0.55 * sin(t * 10.3 + phase * 1.71)
		+ 0.25 * sin(t * 17.7 + phase * 0.63);
	return 1.0 + 0.020 * irregular;
}

void vertex() {
	vec3 camera_world = INV_VIEW_MATRIX[3].xyz;
	vec3 centre_world = (MODEL_MATRIX * vec4(vec3(0.0), 1.0)).xyz;
	v_star_dir = normalize(centre_world - camera_world);

	// The quad is only the renderer's point-spread sampling footprint. Every star
	// gets the same size; bright stars are not enlarged for presentation.
	float sx = length(MODEL_MATRIX[0].xyz);
	float sy = length(MODEL_MATRIX[1].xyz);
	vec3 camera_right = normalize(INV_VIEW_MATRIX[0].xyz);
	vec3 camera_up = normalize(INV_VIEW_MATRIX[1].xyz);
	vec3 world_vertex = centre_world
		+ camera_right * VERTEX.x * sx
		+ camera_up * VERTEX.y * sy;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * vec4(world_vertex, 1.0);

	v_magnitude_norm = INSTANCE_CUSTOM.x;
	v_temperature = INSTANCE_CUSTOM.y;
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
	float atmosphere_presence = 1.0 - smoothstep(
		atmosphere_height * 0.72,
		atmosphere_height * 1.08,
		u_camera_height
	);

	// Physical atmospheric loss only. There is deliberately no sun-altitude,
	// 'night visibility', bright-star reveal, or exposure-compensation multiplier.
	float elevation = dot(normalize(v_star_dir), up);
	float above_horizon = smoothstep(-0.012, 0.045, elevation);
	float air_mass = 1.0 / max(elevation, 0.055);
	float extinction = exp(-0.060 * max(air_mass - 1.0, 0.0));
	float atmospheric_transmittance = mix(
		1.0,
		above_horizon * extinction,
		atmosphere_presence
	);

	float kind = floor(v_pattern * 4.0 + 0.5);
	float speed = mix(0.78, 1.34, fract(v_phase * 13.731));
	float raw_twinkle = twinkle_pattern(kind, TIME * speed, v_phase * 6.28318530718);
	float near_horizon = 1.0 - smoothstep(0.12, 0.82, max(elevation, 0.0));
	float scintillation_strength = atmosphere_presence * mix(0.18, 1.0, near_horizon);
	float scintillation = mix(1.0, raw_twinkle, scintillation_strength);

	float radiance = stellar_radiance(v_magnitude_norm);
	EMISSION = stellar_colour(v_temperature) * radiance * scintillation;
	ALPHA = profile * atmospheric_transmittance;
}
