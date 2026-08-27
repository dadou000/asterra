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

float terrain_persistent_edit_delta(vec3 dir, float planet_radius) {
	if (u_edit_ready < 0.5 || u_edit_half_extent_m <= 0.5) return 0.0;
	vec3 d = normalize(dir);
	float denom = dot(d, normalize(u_edit_center_dir));
	if (denom <= 0.01) return 0.0;
	vec2 plane_m = vec2(dot(d, normalize(u_edit_center_right)),
		dot(d, normalize(u_edit_center_up))) / denom * planet_radius;
	vec2 uv = plane_m / (u_edit_half_extent_m * 2.0) + vec2(0.5);
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 0.0;
	return textureLod(u_edit_delta, uv, 0.0).r;
}

float terrain_active_deformation_delta(vec3 dir, float planet_radius) {
	if (u_active_deform_ready < 0.5 || u_active_deform_half_extent_m <= 0.5) return 0.0;
	vec3 d = normalize(dir);
	float denom = dot(d, normalize(u_active_deform_center_dir));
	if (denom <= 0.01) return 0.0;
	vec2 plane_m = vec2(dot(d, normalize(u_active_deform_center_right)),
		dot(d, normalize(u_active_deform_center_up))) / denom * planet_radius;
	vec2 uv = plane_m / (u_active_deform_half_extent_m * 2.0) + vec2(0.5);
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 0.0;
	return textureLod(u_active_deform, uv, 0.0).r;
}

float terrain_edit_delta(vec3 dir, float planet_radius) {
	return terrain_persistent_edit_delta(dir, planet_radius)
		+ terrain_active_deformation_delta(dir, planet_radius);
}
