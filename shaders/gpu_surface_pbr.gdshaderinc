// Precision-safe scanned PBR detail for the GPU-first surface classifier.
//
// The classifier remains authoritative. These scans add resolvable colour,
// roughness and normal structure only where the camera can see it. The floating
// origin is reduced modulo 4096 m on the CPU. Every material period below divides
// 4096 exactly, so crossing the wrap cannot change texture phase.
//
// All samples use textureGrad(). Material selection is intentionally divergent,
// so implicit derivatives inside those branches are not reliable. The caller
// computes dFdx/dFdy of the precision-safe detail position before branching and
// passes them through this file for explicit anisotropic mip selection.

uniform sampler2D u_pbr_ground_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_pbr_ground_normal : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_pbr_ground_roughness : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_pbr_grass_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_pbr_grass_normal : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_pbr_grass_roughness : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_pbr_mud_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_pbr_mud_normal : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_pbr_mud_roughness : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_pbr_forest_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_pbr_forest_normal : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_pbr_forest_roughness : filter_linear_mipmap_anisotropic, repeat_enable;

uniform vec3 u_detail_origin = vec3(0.0);
uniform float u_pbr_enabled = 1.0;
uniform float u_pbr_ground_metres : hint_range(0.02, 1024.0) = 2.0;
uniform float u_pbr_grass_metres : hint_range(0.02, 1024.0) = 2.0;
uniform float u_pbr_mud_metres : hint_range(0.02, 1024.0) = 1.0;
uniform float u_pbr_forest_metres : hint_range(0.02, 1024.0) = 2.0;
uniform float u_pbr_triplanar_sharpness : hint_range(1.0, 16.0) = 5.0;
uniform float u_pbr_transfer_strength : hint_range(0.0, 2.0) = 0.60;
uniform float u_pbr_transfer_min : hint_range(0.0, 2.0) = 0.48;
uniform float u_pbr_transfer_max : hint_range(0.0, 4.0) = 1.72;
uniform float u_pbr_surface_fade_near_m : hint_range(0.0, 20000.0) = 900.0;
uniform float u_pbr_surface_fade_far_m : hint_range(0.0, 50000.0) = 3200.0;
uniform float u_pbr_normal_fade_near_m : hint_range(0.0, 10000.0) = 120.0;
uniform float u_pbr_normal_fade_far_m : hint_range(0.0, 20000.0) = 520.0;
uniform float u_pbr_loose_threshold : hint_range(0.0, 1.0) = 0.015;
uniform float u_pbr_loose_albedo_strength : hint_range(0.0, 2.0) = 0.72;
uniform float u_pbr_loose_roughness_strength : hint_range(0.0, 2.0) = 0.70;
uniform float u_pbr_loose_normal_mix : hint_range(0.0, 2.0) = 0.42;
uniform float u_pbr_loose_normal_weight : hint_range(0.0, 2.0) = 0.48;
uniform float u_pbr_special_threshold : hint_range(0.0, 1.0) = 0.015;
uniform float u_pbr_special_albedo_strength : hint_range(0.0, 2.0) = 0.88;
uniform float u_pbr_special_roughness_strength : hint_range(0.0, 2.0) = 0.82;
uniform float u_pbr_special_normal_mix : hint_range(0.0, 2.0) = 0.58;
uniform float u_pbr_special_normal_weight : hint_range(0.0, 2.0) = 0.72;

#define SPBR_GROUND_M u_pbr_ground_metres
#define SPBR_GRASS_M u_pbr_grass_metres
#define SPBR_MUD_M u_pbr_mud_metres
#define SPBR_FOREST_M u_pbr_forest_metres

vec3 spbr_weights(vec3 n) {
	vec3 w = pow(abs(n), vec3(max(u_pbr_triplanar_sharpness, 0.01)));
	return w / max(w.x + w.y + w.z, 1e-5);
}

vec3 spbr_axis_sign(vec3 n) {
	vec3 s = sign(n);
	return mix(vec3(1.0), s, step(vec3(1e-5), abs(n)));
}

vec2 spbr_uv_x(vec3 p, float metres, float sx) {
	return vec2(-sx * p.z, p.y) / metres;
}

vec2 spbr_uv_y(vec3 p, float metres, float sy) {
	return vec2(p.x, -sy * p.z) / metres;
}

vec2 spbr_uv_z(vec3 p, float metres, float sz) {
	return vec2(sz * p.x, p.y) / metres;
}

vec2 spbr_grad_x(vec3 dp, float metres, float sx) {
	return vec2(-sx * dp.z, dp.y) / metres;
}

vec2 spbr_grad_y(vec3 dp, float metres, float sy) {
	return vec2(dp.x, -sy * dp.z) / metres;
}

vec2 spbr_grad_z(vec3 dp, float metres, float sz) {
	return vec2(sz * dp.x, dp.y) / metres;
}

vec3 spbr_tri_albedo_ground(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	return textureGrad(u_pbr_ground_albedo,
		spbr_uv_x(p, SPBR_GROUND_M, s.x),
		spbr_grad_x(dpdx, SPBR_GROUND_M, s.x), spbr_grad_x(dpdy, SPBR_GROUND_M, s.x)).rgb * w.x
		+ textureGrad(u_pbr_ground_albedo,
			spbr_uv_y(p, SPBR_GROUND_M, s.y),
			spbr_grad_y(dpdx, SPBR_GROUND_M, s.y), spbr_grad_y(dpdy, SPBR_GROUND_M, s.y)).rgb * w.y
		+ textureGrad(u_pbr_ground_albedo,
			spbr_uv_z(p, SPBR_GROUND_M, s.z),
			spbr_grad_z(dpdx, SPBR_GROUND_M, s.z), spbr_grad_z(dpdy, SPBR_GROUND_M, s.z)).rgb * w.z;
}

float spbr_tri_rough_ground(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	return textureGrad(u_pbr_ground_roughness,
		spbr_uv_x(p, SPBR_GROUND_M, s.x),
		spbr_grad_x(dpdx, SPBR_GROUND_M, s.x), spbr_grad_x(dpdy, SPBR_GROUND_M, s.x)).r * w.x
		+ textureGrad(u_pbr_ground_roughness,
			spbr_uv_y(p, SPBR_GROUND_M, s.y),
			spbr_grad_y(dpdx, SPBR_GROUND_M, s.y), spbr_grad_y(dpdy, SPBR_GROUND_M, s.y)).r * w.y
		+ textureGrad(u_pbr_ground_roughness,
			spbr_uv_z(p, SPBR_GROUND_M, s.z),
			spbr_grad_z(dpdx, SPBR_GROUND_M, s.z), spbr_grad_z(dpdy, SPBR_GROUND_M, s.z)).r * w.z;
}

vec3 spbr_tri_albedo_grass(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	return textureGrad(u_pbr_grass_albedo,
		spbr_uv_x(p, SPBR_GRASS_M, s.x),
		spbr_grad_x(dpdx, SPBR_GRASS_M, s.x), spbr_grad_x(dpdy, SPBR_GRASS_M, s.x)).rgb * w.x
		+ textureGrad(u_pbr_grass_albedo,
			spbr_uv_y(p, SPBR_GRASS_M, s.y),
			spbr_grad_y(dpdx, SPBR_GRASS_M, s.y), spbr_grad_y(dpdy, SPBR_GRASS_M, s.y)).rgb * w.y
		+ textureGrad(u_pbr_grass_albedo,
			spbr_uv_z(p, SPBR_GRASS_M, s.z),
			spbr_grad_z(dpdx, SPBR_GRASS_M, s.z), spbr_grad_z(dpdy, SPBR_GRASS_M, s.z)).rgb * w.z;
}

float spbr_tri_rough_grass(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	return textureGrad(u_pbr_grass_roughness,
		spbr_uv_x(p, SPBR_GRASS_M, s.x),
		spbr_grad_x(dpdx, SPBR_GRASS_M, s.x), spbr_grad_x(dpdy, SPBR_GRASS_M, s.x)).r * w.x
		+ textureGrad(u_pbr_grass_roughness,
			spbr_uv_y(p, SPBR_GRASS_M, s.y),
			spbr_grad_y(dpdx, SPBR_GRASS_M, s.y), spbr_grad_y(dpdy, SPBR_GRASS_M, s.y)).r * w.y
		+ textureGrad(u_pbr_grass_roughness,
			spbr_uv_z(p, SPBR_GRASS_M, s.z),
			spbr_grad_z(dpdx, SPBR_GRASS_M, s.z), spbr_grad_z(dpdy, SPBR_GRASS_M, s.z)).r * w.z;
}

vec3 spbr_tri_albedo_mud(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	return textureGrad(u_pbr_mud_albedo,
		spbr_uv_x(p, SPBR_MUD_M, s.x),
		spbr_grad_x(dpdx, SPBR_MUD_M, s.x), spbr_grad_x(dpdy, SPBR_MUD_M, s.x)).rgb * w.x
		+ textureGrad(u_pbr_mud_albedo,
			spbr_uv_y(p, SPBR_MUD_M, s.y),
			spbr_grad_y(dpdx, SPBR_MUD_M, s.y), spbr_grad_y(dpdy, SPBR_MUD_M, s.y)).rgb * w.y
		+ textureGrad(u_pbr_mud_albedo,
			spbr_uv_z(p, SPBR_MUD_M, s.z),
			spbr_grad_z(dpdx, SPBR_MUD_M, s.z), spbr_grad_z(dpdy, SPBR_MUD_M, s.z)).rgb * w.z;
}

float spbr_tri_rough_mud(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	return textureGrad(u_pbr_mud_roughness,
		spbr_uv_x(p, SPBR_MUD_M, s.x),
		spbr_grad_x(dpdx, SPBR_MUD_M, s.x), spbr_grad_x(dpdy, SPBR_MUD_M, s.x)).r * w.x
		+ textureGrad(u_pbr_mud_roughness,
			spbr_uv_y(p, SPBR_MUD_M, s.y),
			spbr_grad_y(dpdx, SPBR_MUD_M, s.y), spbr_grad_y(dpdy, SPBR_MUD_M, s.y)).r * w.y
		+ textureGrad(u_pbr_mud_roughness,
			spbr_uv_z(p, SPBR_MUD_M, s.z),
			spbr_grad_z(dpdx, SPBR_MUD_M, s.z), spbr_grad_z(dpdy, SPBR_MUD_M, s.z)).r * w.z;
}

vec3 spbr_tri_albedo_forest(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	return textureGrad(u_pbr_forest_albedo,
		spbr_uv_x(p, SPBR_FOREST_M, s.x),
		spbr_grad_x(dpdx, SPBR_FOREST_M, s.x), spbr_grad_x(dpdy, SPBR_FOREST_M, s.x)).rgb * w.x
		+ textureGrad(u_pbr_forest_albedo,
			spbr_uv_y(p, SPBR_FOREST_M, s.y),
			spbr_grad_y(dpdx, SPBR_FOREST_M, s.y), spbr_grad_y(dpdy, SPBR_FOREST_M, s.y)).rgb * w.y
		+ textureGrad(u_pbr_forest_albedo,
			spbr_uv_z(p, SPBR_FOREST_M, s.z),
			spbr_grad_z(dpdx, SPBR_FOREST_M, s.z), spbr_grad_z(dpdy, SPBR_FOREST_M, s.z)).rgb * w.z;
}

float spbr_tri_rough_forest(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	return textureGrad(u_pbr_forest_roughness,
		spbr_uv_x(p, SPBR_FOREST_M, s.x),
		spbr_grad_x(dpdx, SPBR_FOREST_M, s.x), spbr_grad_x(dpdy, SPBR_FOREST_M, s.x)).r * w.x
		+ textureGrad(u_pbr_forest_roughness,
			spbr_uv_y(p, SPBR_FOREST_M, s.y),
			spbr_grad_y(dpdx, SPBR_FOREST_M, s.y), spbr_grad_y(dpdy, SPBR_FOREST_M, s.y)).r * w.y
		+ textureGrad(u_pbr_forest_roughness,
			spbr_uv_z(p, SPBR_FOREST_M, s.z),
			spbr_grad_z(dpdx, SPBR_FOREST_M, s.z), spbr_grad_z(dpdy, SPBR_FOREST_M, s.z)).r * w.z;
}

vec3 spbr_normal_world(vec3 nts, int axis, float sign_axis) {
	if (axis == 0) {
		vec3 t = vec3(0.0, 0.0, -sign_axis);
		vec3 b = vec3(0.0, 1.0, 0.0);
		vec3 nn = vec3(sign_axis, 0.0, 0.0);
		return normalize(t * nts.x + b * nts.y + nn * nts.z);
	}
	if (axis == 1) {
		vec3 t = vec3(1.0, 0.0, 0.0);
		vec3 b = vec3(0.0, 0.0, -sign_axis);
		vec3 nn = vec3(0.0, sign_axis, 0.0);
		return normalize(t * nts.x + b * nts.y + nn * nts.z);
	}
	vec3 t = vec3(sign_axis, 0.0, 0.0);
	vec3 b = vec3(0.0, 1.0, 0.0);
	vec3 nn = vec3(0.0, 0.0, sign_axis);
	return normalize(t * nts.x + b * nts.y + nn * nts.z);
}

vec3 spbr_tri_normal_ground(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	vec3 nx = textureGrad(u_pbr_ground_normal,
		spbr_uv_x(p, SPBR_GROUND_M, s.x),
		spbr_grad_x(dpdx, SPBR_GROUND_M, s.x), spbr_grad_x(dpdy, SPBR_GROUND_M, s.x)).xyz * 2.0 - 1.0;
	vec3 ny = textureGrad(u_pbr_ground_normal,
		spbr_uv_y(p, SPBR_GROUND_M, s.y),
		spbr_grad_y(dpdx, SPBR_GROUND_M, s.y), spbr_grad_y(dpdy, SPBR_GROUND_M, s.y)).xyz * 2.0 - 1.0;
	vec3 nz = textureGrad(u_pbr_ground_normal,
		spbr_uv_z(p, SPBR_GROUND_M, s.z),
		spbr_grad_z(dpdx, SPBR_GROUND_M, s.z), spbr_grad_z(dpdy, SPBR_GROUND_M, s.z)).xyz * 2.0 - 1.0;
	return normalize(spbr_normal_world(nx, 0, s.x) * w.x
		+ spbr_normal_world(ny, 1, s.y) * w.y
		+ spbr_normal_world(nz, 2, s.z) * w.z);
}

vec3 spbr_tri_normal_grass(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	vec3 nx = textureGrad(u_pbr_grass_normal,
		spbr_uv_x(p, SPBR_GRASS_M, s.x),
		spbr_grad_x(dpdx, SPBR_GRASS_M, s.x), spbr_grad_x(dpdy, SPBR_GRASS_M, s.x)).xyz * 2.0 - 1.0;
	vec3 ny = textureGrad(u_pbr_grass_normal,
		spbr_uv_y(p, SPBR_GRASS_M, s.y),
		spbr_grad_y(dpdx, SPBR_GRASS_M, s.y), spbr_grad_y(dpdy, SPBR_GRASS_M, s.y)).xyz * 2.0 - 1.0;
	vec3 nz = textureGrad(u_pbr_grass_normal,
		spbr_uv_z(p, SPBR_GRASS_M, s.z),
		spbr_grad_z(dpdx, SPBR_GRASS_M, s.z), spbr_grad_z(dpdy, SPBR_GRASS_M, s.z)).xyz * 2.0 - 1.0;
	return normalize(spbr_normal_world(nx, 0, s.x) * w.x
		+ spbr_normal_world(ny, 1, s.y) * w.y
		+ spbr_normal_world(nz, 2, s.z) * w.z);
}

vec3 spbr_tri_normal_mud(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	vec3 nx = textureGrad(u_pbr_mud_normal,
		spbr_uv_x(p, SPBR_MUD_M, s.x),
		spbr_grad_x(dpdx, SPBR_MUD_M, s.x), spbr_grad_x(dpdy, SPBR_MUD_M, s.x)).xyz * 2.0 - 1.0;
	vec3 ny = textureGrad(u_pbr_mud_normal,
		spbr_uv_y(p, SPBR_MUD_M, s.y),
		spbr_grad_y(dpdx, SPBR_MUD_M, s.y), spbr_grad_y(dpdy, SPBR_MUD_M, s.y)).xyz * 2.0 - 1.0;
	vec3 nz = textureGrad(u_pbr_mud_normal,
		spbr_uv_z(p, SPBR_MUD_M, s.z),
		spbr_grad_z(dpdx, SPBR_MUD_M, s.z), spbr_grad_z(dpdy, SPBR_MUD_M, s.z)).xyz * 2.0 - 1.0;
	return normalize(spbr_normal_world(nx, 0, s.x) * w.x
		+ spbr_normal_world(ny, 1, s.y) * w.y
		+ spbr_normal_world(nz, 2, s.z) * w.z);
}

vec3 spbr_tri_normal_forest(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	vec3 nx = textureGrad(u_pbr_forest_normal,
		spbr_uv_x(p, SPBR_FOREST_M, s.x),
		spbr_grad_x(dpdx, SPBR_FOREST_M, s.x), spbr_grad_x(dpdy, SPBR_FOREST_M, s.x)).xyz * 2.0 - 1.0;
	vec3 ny = textureGrad(u_pbr_forest_normal,
		spbr_uv_y(p, SPBR_FOREST_M, s.y),
		spbr_grad_y(dpdx, SPBR_FOREST_M, s.y), spbr_grad_y(dpdy, SPBR_FOREST_M, s.y)).xyz * 2.0 - 1.0;
	vec3 nz = textureGrad(u_pbr_forest_normal,
		spbr_uv_z(p, SPBR_FOREST_M, s.z),
		spbr_grad_z(dpdx, SPBR_FOREST_M, s.z), spbr_grad_z(dpdy, SPBR_FOREST_M, s.z)).xyz * 2.0 - 1.0;
	return normalize(spbr_normal_world(nx, 0, s.x) * w.x
		+ spbr_normal_world(ny, 1, s.y) * w.y
		+ spbr_normal_world(nz, 2, s.z) * w.z);
}

float spbr_forest_biome(float biome) {
	return max(max(sc_is_biome(biome, 5.0), sc_is_biome(biome, 8.0)),
		max(max(sc_is_biome(biome, 9.0), sc_is_biome(biome, 14.0)), sc_is_biome(biome, 15.0)));
}

vec3 spbr_transfer_detail(vec3 base, vec3 scan) {
	float lum = max(dot(scan, vec3(0.2126, 0.7152, 0.0722)), 0.035);
	float relative_min = min(u_pbr_transfer_min, u_pbr_transfer_max);
	float relative_max = max(u_pbr_transfer_min, u_pbr_transfer_max);
	vec3 relative = clamp(scan / lum, vec3(relative_min), vec3(relative_max));
	return max(base * mix(vec3(1.0), relative, u_pbr_transfer_strength), vec3(0.006));
}

void spbr_apply_grad(vec3 dpos, vec3 dpdx, vec3 dpdy, vec3 geom_n, float camera_dist,
		vec4 primary, vec4 secondary, float biome,
		vec3 base_albedo, float base_roughness,
		out vec3 albedo, out float roughness, out vec3 pbr_normal, out float pbr_normal_w) {
	albedo = base_albedo;
	roughness = base_roughness;
	pbr_normal = geom_n;
	pbr_normal_w = 0.0;
	if (u_pbr_enabled < 0.5) return;

	float surface_near = min(u_pbr_surface_fade_near_m, u_pbr_surface_fade_far_m);
	float surface_far = max(u_pbr_surface_fade_near_m, u_pbr_surface_fade_far_m);
	float normal_near = min(u_pbr_normal_fade_near_m, u_pbr_normal_fade_far_m);
	float normal_far = max(u_pbr_normal_fade_near_m, u_pbr_normal_fade_far_m);
	float surface_detail_w = 1.0 - smoothstep(surface_near, max(surface_far, surface_near + 0.001), camera_dist);
	if (surface_detail_w <= 0.002) return;
	float normal_detail_w = 1.0 - smoothstep(normal_near, max(normal_far, normal_near + 0.001), camera_dist);

	float snow = secondary.g;
	float rock = primary.r;
	float loose = clamp(primary.g + primary.a * 0.82 + secondary.b * 0.68 + secondary.a * 0.78,
		0.0, 1.0) * (1.0 - snow) * (1.0 - rock * 0.88);

	if (loose > u_pbr_loose_threshold) {
		vec3 scan = spbr_tri_albedo_ground(dpos, dpdx, dpdy, geom_n);
		float scan_r = spbr_tri_rough_ground(dpos, dpdx, dpdy, geom_n);
		float w = loose * surface_detail_w * u_pbr_loose_albedo_strength;
		albedo = mix(albedo, spbr_transfer_detail(albedo, scan), clamp(w, 0.0, 1.0));
		roughness = mix(roughness, scan_r, clamp(w * u_pbr_loose_roughness_strength, 0.0, 1.0));
		if (normal_detail_w > 0.002) {
			vec3 sn = spbr_tri_normal_ground(dpos, dpdx, dpdy, geom_n);
			pbr_normal = normalize(mix(pbr_normal, sn, clamp(loose * u_pbr_loose_normal_mix, 0.0, 1.0)));
			pbr_normal_w = max(pbr_normal_w,
				clamp(loose * normal_detail_w * u_pbr_loose_normal_weight, 0.0, 1.0));
		}
	}

	float forest_biome = spbr_forest_biome(biome);
	float forest = primary.b * forest_biome * (1.0 - snow);
	float grass = primary.b * (1.0 - forest_biome) * (1.0 - snow);
	float mud = secondary.r * (1.0 - snow);
	float special = max(max(grass, forest), mud);
	if (special > u_pbr_special_threshold) {
		vec3 scan;
		float scan_r;
		vec3 scan_n = geom_n;
		if (forest >= grass && forest >= mud) {
			scan = spbr_tri_albedo_forest(dpos, dpdx, dpdy, geom_n);
			scan_r = spbr_tri_rough_forest(dpos, dpdx, dpdy, geom_n);
			if (normal_detail_w > 0.002) scan_n = spbr_tri_normal_forest(dpos, dpdx, dpdy, geom_n);
		} else if (mud >= grass) {
			scan = spbr_tri_albedo_mud(dpos, dpdx, dpdy, geom_n);
			scan_r = spbr_tri_rough_mud(dpos, dpdx, dpdy, geom_n);
			if (normal_detail_w > 0.002) scan_n = spbr_tri_normal_mud(dpos, dpdx, dpdy, geom_n);
		} else {
			scan = spbr_tri_albedo_grass(dpos, dpdx, dpdy, geom_n);
			scan_r = spbr_tri_rough_grass(dpos, dpdx, dpdy, geom_n);
			if (normal_detail_w > 0.002) scan_n = spbr_tri_normal_grass(dpos, dpdx, dpdy, geom_n);
		}
		float w = special * surface_detail_w * u_pbr_special_albedo_strength;
		albedo = mix(albedo, spbr_transfer_detail(albedo, scan), clamp(w, 0.0, 1.0));
		roughness = mix(roughness, scan_r, clamp(w * u_pbr_special_roughness_strength, 0.0, 1.0));
		if (normal_detail_w > 0.002) {
			pbr_normal = normalize(mix(pbr_normal, scan_n,
				clamp(special * u_pbr_special_normal_mix, 0.0, 1.0)));
			pbr_normal_w = max(pbr_normal_w,
				clamp(special * normal_detail_w * u_pbr_special_normal_weight, 0.0, 1.0));
		}
	}
}

// Compatibility entry point used by the active terrain shader. dFdx/dFdy are
// evaluated here before any material-dependent branch in spbr_apply_grad().
void spbr_apply(vec3 dpos, vec3 geom_n, float camera_dist,
		vec4 primary, vec4 secondary, float biome,
		vec3 base_albedo, float base_roughness,
		out vec3 albedo, out float roughness, out vec3 pbr_normal, out float pbr_normal_w) {
	vec3 dpdx = dFdx(dpos);
	vec3 dpdy = dFdy(dpos);
	spbr_apply_grad(dpos, dpdx, dpdy, geom_n, camera_dist,
		primary, secondary, biome, base_albedo, base_roughness,
		albedo, roughness, pbr_normal, pbr_normal_w);
}
