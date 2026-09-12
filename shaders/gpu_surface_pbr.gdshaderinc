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
uniform float u_pbr_ground_macro_metres : hint_range(1.0, 256.0) = 16.0;
uniform float u_pbr_grass_macro_metres : hint_range(1.0, 256.0) = 14.0;
uniform float u_pbr_mud_macro_metres : hint_range(1.0, 256.0) = 10.0;
uniform float u_pbr_forest_macro_metres : hint_range(1.0, 256.0) = 18.0;
uniform float u_pbr_micro_macro_near_m : hint_range(0.0, 500.0) = 4.0;
uniform float u_pbr_micro_macro_far_m : hint_range(0.0, 2000.0) = 60.0;
uniform float u_pbr_macro_strength : hint_range(0.0, 2.0) = 1.0;
uniform float u_pbr_triplanar_sharpness : hint_range(1.0, 16.0) = 5.0;
// Optimization controls. Dominant surfaces use one projection; blended surfaces
// use the two strongest projections. The discarded axis was already contributing
// the least and weights are renormalized, preserving the triplanar transition
// while cutting one to two textureGrad calls per property.
uniform float u_pbr_triplanar_single_axis_threshold : hint_range(0.50, 0.999) = 0.92;
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

vec3 spbr_sample_albedo_axis(sampler2D tex, vec3 p, vec3 dpdx, vec3 dpdy,
		float metres, vec3 s, int axis) {
	if (axis == 0) return textureGrad(tex, spbr_uv_x(p, metres, s.x),
		spbr_grad_x(dpdx, metres, s.x), spbr_grad_x(dpdy, metres, s.x)).rgb;
	if (axis == 1) return textureGrad(tex, spbr_uv_y(p, metres, s.y),
		spbr_grad_y(dpdx, metres, s.y), spbr_grad_y(dpdy, metres, s.y)).rgb;
	return textureGrad(tex, spbr_uv_z(p, metres, s.z),
		spbr_grad_z(dpdx, metres, s.z), spbr_grad_z(dpdy, metres, s.z)).rgb;
}

float spbr_sample_rough_axis(sampler2D tex, vec3 p, vec3 dpdx, vec3 dpdy,
		float metres, vec3 s, int axis) {
	if (axis == 0) return textureGrad(tex, spbr_uv_x(p, metres, s.x),
		spbr_grad_x(dpdx, metres, s.x), spbr_grad_x(dpdy, metres, s.x)).r;
	if (axis == 1) return textureGrad(tex, spbr_uv_y(p, metres, s.y),
		spbr_grad_y(dpdx, metres, s.y), spbr_grad_y(dpdy, metres, s.y)).r;
	return textureGrad(tex, spbr_uv_z(p, metres, s.z),
		spbr_grad_z(dpdx, metres, s.z), spbr_grad_z(dpdy, metres, s.z)).r;
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

vec3 spbr_sample_normal_axis(sampler2D tex, vec3 p, vec3 dpdx, vec3 dpdy,
		float metres, vec3 s, int axis) {
	vec3 nts;
	if (axis == 0) {
		nts = textureGrad(tex, spbr_uv_x(p, metres, s.x),
			spbr_grad_x(dpdx, metres, s.x), spbr_grad_x(dpdy, metres, s.x)).xyz * 2.0 - 1.0;
		return spbr_normal_world(nts, 0, s.x);
	}
	if (axis == 1) {
		nts = textureGrad(tex, spbr_uv_y(p, metres, s.y),
			spbr_grad_y(dpdx, metres, s.y), spbr_grad_y(dpdy, metres, s.y)).xyz * 2.0 - 1.0;
		return spbr_normal_world(nts, 1, s.y);
	}
	nts = textureGrad(tex, spbr_uv_z(p, metres, s.z),
		spbr_grad_z(dpdx, metres, s.z), spbr_grad_z(dpdy, metres, s.z)).xyz * 2.0 - 1.0;
	return spbr_normal_world(nts, 2, s.z);
}

void spbr_top_two_axes(vec3 w, out int a, out int b, out float wa, out float wb) {
	if (w.x >= w.y && w.x >= w.z) {
		a = 0;
		if (w.y >= w.z) { b = 1; wa = w.x; wb = w.y; }
		else { b = 2; wa = w.x; wb = w.z; }
	} else if (w.y >= w.z) {
		a = 1;
		if (w.x >= w.z) { b = 0; wa = w.y; wb = w.x; }
		else { b = 2; wa = w.y; wb = w.z; }
	} else {
		a = 2;
		if (w.x >= w.y) { b = 0; wa = w.z; wb = w.x; }
		else { b = 1; wa = w.z; wb = w.y; }
	}
	float inv_sum = 1.0 / max(wa + wb, 1e-5);
	wa *= inv_sum;
	wb *= inv_sum;
}

vec3 spbr_tri_albedo_at(sampler2D tex, vec3 p, vec3 dpdx, vec3 dpdy, vec3 n, float metres) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	int a;
	int b;
	float wa;
	float wb;
	spbr_top_two_axes(w, a, b, wa, wb);
	if (wa >= u_pbr_triplanar_single_axis_threshold) {
		return spbr_sample_albedo_axis(tex, p, dpdx, dpdy, metres, s, a);
	}
	return spbr_sample_albedo_axis(tex, p, dpdx, dpdy, metres, s, a) * wa
		+ spbr_sample_albedo_axis(tex, p, dpdx, dpdy, metres, s, b) * wb;
}

float spbr_tri_rough_at(sampler2D tex, vec3 p, vec3 dpdx, vec3 dpdy, vec3 n, float metres) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	int a;
	int b;
	float wa;
	float wb;
	spbr_top_two_axes(w, a, b, wa, wb);
	if (wa >= u_pbr_triplanar_single_axis_threshold) {
		return spbr_sample_rough_axis(tex, p, dpdx, dpdy, metres, s, a);
	}
	return spbr_sample_rough_axis(tex, p, dpdx, dpdy, metres, s, a) * wa
		+ spbr_sample_rough_axis(tex, p, dpdx, dpdy, metres, s, b) * wb;
}

vec3 spbr_tri_normal_at(sampler2D tex, vec3 p, vec3 dpdx, vec3 dpdy, vec3 n, float metres) {
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	int a;
	int b;
	float wa;
	float wb;
	spbr_top_two_axes(w, a, b, wa, wb);
	vec3 na = spbr_sample_normal_axis(tex, p, dpdx, dpdy, metres, s, a);
	if (wa >= u_pbr_triplanar_single_axis_threshold) return normalize(na);
	vec3 nb = spbr_sample_normal_axis(tex, p, dpdx, dpdy, metres, s, b);
	return normalize(na * wa + nb * wb);
}

vec3 spbr_tri_albedo_ground(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	return spbr_tri_albedo_at(u_pbr_ground_albedo, p, dpdx, dpdy, n, SPBR_GROUND_M);
}

float spbr_tri_rough_ground(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	return spbr_tri_rough_at(u_pbr_ground_roughness, p, dpdx, dpdy, n, SPBR_GROUND_M);
}

vec3 spbr_tri_albedo_grass(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	return spbr_tri_albedo_at(u_pbr_grass_albedo, p, dpdx, dpdy, n, SPBR_GRASS_M);
}

float spbr_tri_rough_grass(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	return spbr_tri_rough_at(u_pbr_grass_roughness, p, dpdx, dpdy, n, SPBR_GRASS_M);
}

vec3 spbr_tri_albedo_mud(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	return spbr_tri_albedo_at(u_pbr_mud_albedo, p, dpdx, dpdy, n, SPBR_MUD_M);
}

float spbr_tri_rough_mud(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	return spbr_tri_rough_at(u_pbr_mud_roughness, p, dpdx, dpdy, n, SPBR_MUD_M);
}

vec3 spbr_tri_albedo_forest(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	return spbr_tri_albedo_at(u_pbr_forest_albedo, p, dpdx, dpdy, n, SPBR_FOREST_M);
}

float spbr_tri_rough_forest(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	return spbr_tri_rough_at(u_pbr_forest_roughness, p, dpdx, dpdy, n, SPBR_FOREST_M);
}

vec3 spbr_tri_normal_ground(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	return spbr_tri_normal_at(u_pbr_ground_normal, p, dpdx, dpdy, n, SPBR_GROUND_M);
}

vec3 spbr_tri_normal_grass(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	return spbr_tri_normal_at(u_pbr_grass_normal, p, dpdx, dpdy, n, SPBR_GRASS_M);
}

vec3 spbr_tri_normal_mud(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	return spbr_tri_normal_at(u_pbr_mud_normal, p, dpdx, dpdy, n, SPBR_MUD_M);
}

vec3 spbr_tri_normal_forest(vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	return spbr_tri_normal_at(u_pbr_forest_normal, p, dpdx, dpdy, n, SPBR_FOREST_M);
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

void spbr_scan_material(sampler2D albedo_tex, sampler2D rough_tex,
		vec3 p, vec3 dpdx, vec3 dpdy, vec3 n, float micro_m, float macro_m, float macro_w,
		out vec3 albedo, out float roughness) {
	albedo = vec3(0.0);
	roughness = 0.0;
	float micro_w = 1.0 - macro_w;
	if (micro_w > 0.002) {
		albedo += spbr_tri_albedo_at(albedo_tex, p, dpdx, dpdy, n, micro_m) * micro_w;
		roughness += spbr_tri_rough_at(rough_tex, p, dpdx, dpdy, n, micro_m) * micro_w;
	}
	if (macro_w > 0.002) {
		vec3 macro_albedo = spbr_tri_albedo_at(albedo_tex, p, dpdx, dpdy, n, macro_m);
		float macro_rough = spbr_tri_rough_at(rough_tex, p, dpdx, dpdy, n, macro_m);
		vec3 macro_flat = vec3(dot(macro_albedo, vec3(0.333333)));
		albedo += mix(macro_flat, macro_albedo, u_pbr_macro_strength) * macro_w;
		roughness += macro_rough * macro_w;
	}
}

vec3 spbr_scan_normal(sampler2D normal_tex, vec3 p, vec3 dpdx, vec3 dpdy, vec3 n,
		float micro_m, float macro_m, float macro_w) {
	if (macro_w <= 0.002) return spbr_tri_normal_at(normal_tex, p, dpdx, dpdy, n, micro_m);
	if (macro_w >= 0.998) return spbr_tri_normal_at(normal_tex, p, dpdx, dpdy, n, macro_m);
	vec3 micro_n = spbr_tri_normal_at(normal_tex, p, dpdx, dpdy, n, micro_m);
	vec3 macro_n = spbr_tri_normal_at(normal_tex, p, dpdx, dpdy, n, macro_m);
	return normalize(mix(micro_n, macro_n, macro_w));
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

	float blend_near = min(u_pbr_micro_macro_near_m, u_pbr_micro_macro_far_m);
	float blend_far = max(u_pbr_micro_macro_near_m, u_pbr_micro_macro_far_m);
	float macro_w = smoothstep(blend_near, max(blend_far, blend_near + 0.001), camera_dist);

	float snow = secondary.g;
	float rock = primary.r;
	float loose = clamp(primary.g + primary.a * 0.82 + secondary.b * 0.68 + secondary.a * 0.78,
		0.0, 1.0) * (1.0 - snow) * (1.0 - rock * 0.88);

	if (loose > u_pbr_loose_threshold) {
		vec3 scan;
		float scan_r;
		spbr_scan_material(u_pbr_ground_albedo, u_pbr_ground_roughness, dpos, dpdx, dpdy, geom_n,
			SPBR_GROUND_M, u_pbr_ground_macro_metres, macro_w, scan, scan_r);
		float w = loose * surface_detail_w * u_pbr_loose_albedo_strength;
		albedo = mix(albedo, spbr_transfer_detail(albedo, scan), clamp(w, 0.0, 1.0));
		roughness = mix(roughness, scan_r, clamp(w * u_pbr_loose_roughness_strength, 0.0, 1.0));
		if (normal_detail_w > 0.002) {
			vec3 sn = spbr_scan_normal(u_pbr_ground_normal, dpos, dpdx, dpdy, geom_n,
				SPBR_GROUND_M, u_pbr_ground_macro_metres, macro_w);
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
			spbr_scan_material(u_pbr_forest_albedo, u_pbr_forest_roughness, dpos, dpdx, dpdy, geom_n,
				SPBR_FOREST_M, u_pbr_forest_macro_metres, macro_w, scan, scan_r);
			if (normal_detail_w > 0.002) scan_n = spbr_scan_normal(u_pbr_forest_normal, dpos, dpdx, dpdy, geom_n,
				SPBR_FOREST_M, u_pbr_forest_macro_metres, macro_w);
		} else if (mud >= grass) {
			spbr_scan_material(u_pbr_mud_albedo, u_pbr_mud_roughness, dpos, dpdx, dpdy, geom_n,
				SPBR_MUD_M, u_pbr_mud_macro_metres, macro_w, scan, scan_r);
			if (normal_detail_w > 0.002) scan_n = spbr_scan_normal(u_pbr_mud_normal, dpos, dpdx, dpdy, geom_n,
				SPBR_MUD_M, u_pbr_mud_macro_metres, macro_w);
		} else {
			spbr_scan_material(u_pbr_grass_albedo, u_pbr_grass_roughness, dpos, dpdx, dpdy, geom_n,
				SPBR_GRASS_M, u_pbr_grass_macro_metres, macro_w, scan, scan_r);
			if (normal_detail_w > 0.002) scan_n = spbr_scan_normal(u_pbr_grass_normal, dpos, dpdx, dpdy, geom_n,
				SPBR_GRASS_M, u_pbr_grass_macro_metres, macro_w);
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
