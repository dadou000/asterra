uniform sampler2D u_edit_delta : filter_linear, repeat_disable;
uniform float u_edit_ready = 0.0;
uniform vec3 u_edit_center_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_edit_center_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_edit_center_up = vec3(0.0, 1.0, 0.0);
uniform float u_edit_half_extent_m = 256.0;

// High-resolution active deformation tile. R stores the live height delta; the
// other channels carry compaction/damage for compute and CPU readback.
uniform sampler2D u_active_deform : filter_linear, repeat_disable;
uniform float u_active_deform_ready = 0.0;
uniform vec3 u_active_deform_center_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_active_deform_center_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_active_deform_center_up = vec3(0.0, 1.0, 0.0);
uniform float u_active_deform_half_extent_m = 32.0;

float terrain_window_sample(vec3 dir, float planet_radius, sampler2D field,
		float ready, vec3 center_dir, vec3 center_right, vec3 center_up,
		float half_extent_m) {
	if (ready < 0.5 || half_extent_m <= 0.5) return 0.0;
	vec3 d = normalize(dir);
	float denom = dot(d, normalize(center_dir));
	if (denom <= 0.01) return 0.0;
	vec2 plane_m = vec2(dot(d, normalize(center_right)),
		dot(d, normalize(center_up))) / denom * planet_radius;
	vec2 uv = plane_m / (half_extent_m * 2.0) + vec2(0.5);
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 0.0;
	return textureLod(field, uv, 0.0).r;
}

float terrain_edit_delta(vec3 dir, float planet_radius) {
	float persistent = terrain_window_sample(dir, planet_radius, u_edit_delta,
		u_edit_ready, u_edit_center_dir, u_edit_center_right, u_edit_center_up,
		u_edit_half_extent_m);
	float active = terrain_window_sample(dir, planet_radius, u_active_deform,
		u_active_deform_ready, u_active_deform_center_dir,
		u_active_deform_center_right, u_active_deform_center_up,
		u_active_deform_half_extent_m);
	return persistent + active;
}
