shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never, blend_add, fog_disabled;

// GPU-instanced unresolved stellar points. The MultiMesh contains a unit-sphere
// catalogue. Its parent follows the camera and is scaled inside the far clip, so
// every star behaves like an object at infinity without any per-frame CPU work.
uniform vec3 u_up = vec3(0.0, 1.0, 0.0);
uniform vec3 u_sun_dir = vec3(0.0, 1.0, 0.0);
uniform float u_camera_height = 0.0;
uniform float u_atmosphere_height = 60000.0;

varying vec3 v_star_dir;
varying float v_brightness;
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

float twinkle_pattern(float kind, float t, float phase) {
	if (kind < 0.5) return 1.0;
	if (kind < 1.5) return 0.94 + 0.06 * sin(t * 5.4 + phase);
	if (kind < 2.5) {
		float a = sin(t * 3.1 + phase);
		float b = sin(t * 8.7 + phase * 2.17);
		return 0.94 + 0.035 * a + 0.025 * b;
	}
	if (kind < 3.5) {
		float pulse = smoothstep(0.55, 0.98, sin(t * 2.2 + phase));
		return 0.93 + 0.07 * pulse;
	}
	float irregular = sin(t * 4.1 + phase)
		+ 0.55 * sin(t * 10.3 + phase * 1.71)
		+ 0.25 * sin(t * 17.7 + phase * 0.63);
	return 0.95 + 0.035 * irregular;
}

void vertex() {
	vec3 camera_world = INV_VIEW_MATRIX[3].xyz;
	vec3 centre_world = (MODEL_MATRIX * vec4(vec3(0.0), 1.0)).xyz;
	v_star_dir = normalize(centre_world - camera_world);
	float sx = length(MODEL_MATRIX[0].xyz);
	float sy = length(MODEL_MATRIX[1].xyz);
	vec3 camera_right = normalize(INV_VIEW_MATRIX[0].xyz);
	vec3 camera_up = normalize(INV_VIEW_MATRIX[1].xyz);
	vec3 world_vertex = centre_world
		+ camera_right * VERTEX.x * sx
		+ camera_up * VERTEX.y * sy;
	POSITION = PROJECTION_MATRIX * VIEW_MATRIX * vec4(world_vertex, 1.0);
	v_brightness = INSTANCE_CUSTOM.x;
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
	vec3 sun_dir = normalize(u_sun_dir);
	float atmosphere_height = max(u_atmosphere_height, 1.0);
	float atmosphere_presence = 1.0 - smoothstep(
		atmosphere_height * 0.72,
		atmosphere_height * 1.08,
		u_camera_height
	);

	float sun_altitude = dot(up, sun_dir);
	float night_visibility = 1.0 - smoothstep(-0.20, -0.025, sun_altitude);
	float bright_star_early = 1.0 - smoothstep(-0.105, -0.012, sun_altitude);
	float brightness_rank = smoothstep(0.10, 0.42, v_brightness);
	night_visibility = mix(
		night_visibility,
		max(night_visibility, bright_star_early),
		brightness_rank * 0.28
	);

	float elevation = dot(normalize(v_star_dir), up);
	float above_horizon = smoothstep(-0.012, 0.045, elevation);
	float air_mass = 1.0 / max(elevation, 0.055);
	float extinction = exp(-0.060 * max(air_mass - 1.0, 0.0));
	float atmospheric_visibility = mix(1.0, above_horizon * extinction, atmosphere_presence);
	float visibility = mix(1.0, night_visibility, atmosphere_presence) * atmospheric_visibility;

	float kind = floor(v_pattern * 4.0 + 0.5);
	float speed = mix(0.78, 1.34, fract(v_phase * 13.731));
	float raw_twinkle = twinkle_pattern(kind, TIME * speed, v_phase * 6.28318530718);
	float near_horizon = 1.0 - smoothstep(0.12, 0.82, max(elevation, 0.0));
	float scintillation = atmosphere_presence * mix(0.18, 1.0, near_horizon);
	float twinkle = mix(1.0, raw_twinkle, scintillation);

	EMISSION = stellar_colour(v_temperature) * v_brightness * twinkle;
	ALPHA = profile * visibility;
}
