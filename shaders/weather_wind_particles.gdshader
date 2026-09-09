shader_type spatial;
render_mode unshaded, blend_add, depth_draw_never, cull_disabled;

uniform sampler2D u_wind : filter_linear, repeat_disable;
uniform float u_display_radius = 1.006;
uniform float u_planet_radius_m = 3500000.0;
// Advection window is model time. Trail time is physical: streak length is
// speed * trail_time / planet_radius, so fast jet-stream particles stay visible
// longer and draw proportionally longer paths.
uniform float u_advection_window_s = 36000.0;
uniform float u_trail_time_s = 1200.0;
uniform float u_animation_rate = 0.14;
uniform float u_min_line_length = 0.0022;
uniform float u_max_line_length = 0.036;
uniform float u_line_width = 0.00125;
uniform float u_speed_reference = 65.0;
uniform float u_opacity : hint_range(0.0, 2.0) = 0.72;

varying float v_speed;
varying float v_life;

vec2 flow_sample(vec2 uv) {
	return textureLod(u_wind, vec2(fract(uv.x), clamp(uv.y, 0.001, 0.999)), 0.0).rg;
}

vec2 advect_step(vec2 uv, float dt) {
	vec2 wind = flow_sample(uv);
	float lat = (0.5 - uv.y) * PI;
	float cos_lat = max(abs(cos(lat)), 0.10);
	uv.x = fract(uv.x + wind.x * dt / (2.0 * PI * u_planet_radius_m * cos_lat));
	uv.y = clamp(uv.y - wind.y * dt / (PI * u_planet_radius_m), 0.001, 0.999);
	return uv;
}

vec3 sphere_direction(vec2 uv) {
	// Wind textures use the native model convention: x=0 is longitude 0 and
	// increases eastward through 2π. The globe product shader applies its own
	// half-turn only when converting the display map UV into this field UV.
	float lon = uv.x * 2.0 * PI;
	float lat = (0.5 - uv.y) * PI;
	float cl = cos(lat);
	return normalize(vec3(cl * cos(lon), sin(lat), cl * sin(lon)));
}

void vertex() {
	vec2 seed_uv = INSTANCE_CUSTOM.rg;
	float phase = INSTANCE_CUSTOM.b;
	float size_jitter = 0.70 + INSTANCE_CUSTOM.a * 0.60;
	vec4 seed_field = textureLod(u_wind, seed_uv, 0.0);
	float seed_speed = length(seed_field.rg);
	float speed01 = clamp(seed_speed / u_speed_reference, 0.0, 1.0);
	float persistence = mix(0.72, 1.75, speed01);
	float life = fract(TIME * u_animation_rate / persistence + phase);
	float travel = life * u_advection_window_s;
	vec2 uv = seed_uv;

	// Four field samples keep tens of thousands of instances cheap while still
	// following synoptic curvature instead of translating straight-line sprites.
	float dt = travel * 0.25;
	uv = advect_step(uv, dt);
	uv = advect_step(uv, dt);
	uv = advect_step(uv, dt);
	uv = advect_step(uv, dt);

	vec4 field = textureLod(u_wind, uv, 0.0);
	vec3 radial = sphere_direction(uv);
	vec3 east = vec3(-radial.z, 0.0, radial.x);
	if (dot(east, east) < 1e-7) east = vec3(1.0, 0.0, 0.0);
	east = normalize(east);
	vec3 north = normalize(cross(east, radial));
	vec3 flow = east * field.r + north * field.g;
	float speed = length(flow);
	flow = speed > 0.02 ? flow / speed : east;
	vec3 side = normalize(cross(radial, flow));

	float trail_length = clamp(speed * u_trail_time_s / u_planet_radius_m,
		u_min_line_length, u_max_line_length);
	float along = VERTEX.x * trail_length * size_jitter;
	float across = VERTEX.y * u_line_width * size_jitter;
	VERTEX = radial * u_display_radius + flow * along + side * across;

	v_speed = speed;
	v_life = sin(life * PI);
}

vec3 speed_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	if (x < 0.30) return mix(vec3(0.20, 0.55, 0.95), vec3(0.12, 0.82, 0.68), x / 0.30);
	if (x < 0.64) return mix(vec3(0.12, 0.82, 0.68), vec3(0.52, 1.0, 0.25), (x - 0.30) / 0.34);
	if (x < 0.84) return mix(vec3(0.52, 1.0, 0.25), vec3(1.0, 0.88, 0.22), (x - 0.64) / 0.20);
	return mix(vec3(1.0, 0.88, 0.22), vec3(1.0, 0.36, 0.18), (x - 0.84) / 0.16);
}

void fragment() {
	vec2 q = UV - vec2(0.5);
	float longitudinal = 1.0 - smoothstep(0.34, 0.50, abs(q.x));
	float transverse = 1.0 - smoothstep(0.10, 0.49, abs(q.y));
	float alpha = longitudinal * transverse * v_life * u_opacity;
	vec3 col = speed_palette(v_speed / u_speed_reference);
	// Additive + `unshaded`: the framebuffer receives ALBEDO * ALPHA, so the
	// streak colour has to be written to ALBEDO rather than EMISSION.
	ALBEDO = col * (0.50 + alpha * 0.85);
	ALPHA = alpha;
}
