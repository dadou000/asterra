shader_type spatial;
render_mode cull_disabled, diffuse_burley, specular_schlick_ggx;

#include "res://shaders/material_clipmap.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/gpu_planet_context.gdshaderinc"
#include "res://shaders/gpu_geomorph.gdshaderinc"
#include "res://shaders/planet_lighting.gdshaderinc"
#include "res://shaders/cloud_shadow.gdshaderinc"

// GPU geomorph extension of the current 0.0.5 global clipmap shader.
// The topology, promoted centre-disc LOD, stationary per-level lattice,
// double-precision anchor reconstruction, macro B-spline filtering and ring
// handoff code below are intentionally kept identical to the latest clipmap.
uniform sampler2DArray u_macro_elevation : filter_linear_mipmap, repeat_disable;
uniform float u_macro_ready = 0.0;
uniform float u_macro_face_res = 0.0;

uniform float u_planet_radius = 1000000.0;
uniform vec3 u_origin = vec3(0.0);
uniform vec3 u_center_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_center_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_center_up = vec3(0.0, 1.0, 0.0);
uniform vec3 u_anchor_render = vec3(0.0);
uniform vec3 u_anchor_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_anchor_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_anchor_up = vec3(0.0, 1.0, 0.0);
uniform vec2 u_lattice_center_plane = vec2(0.0);
uniform float u_base_spacing = 0.75;
uniform float u_grid_cells = 400.0;
uniform float u_visible_cap_angle = 1.5707963;
uniform float u_surface_bias = 0.035;
uniform float u_height_enabled = 1.0;
uniform float u_sink_scale = 2.0;
uniform int u_detail_seed = 1337;
uniform float u_detail_strength = 1.0;
uniform float u_debug_side_cut = 0.0;
uniform float u_stable_displacement = 1.0;

varying vec3 v_planet;
varying vec3 v_render;
varying vec3 v_smooth_normal;
varying float v_valid;
varying float v_cap_valid;
varying float v_level;
varying float v_direct_transmittance;

const int ASTERRA_MAX_LEVEL = 14;
const float RING_INNER_Q = 0.44;
const float RING_HANDOFF_Q = 0.50;
const float FINE_MORPH_START_Q = 0.875;
const float MAX_PROJECTED_THETA = 1.62;
const float TANGENT_APPROX_MAX_ARC_M = 90000.0;
const float MACRO_FILTER_FOOTPRINT_VERTICES = 3.0;
const float MACRO_MAX_LOD = 5.0;
const int MACRO_MAX_MIP = 5;
const float NORMAL_SAMPLE_SPACING_MULT = 3.0;
const float NORMAL_SAMPLE_MIN_M = 4.0;

float ring_spacing(int level) {
	return u_base_spacing * exp2(float(level));
}

float smootherstep01(float x) {
	x = clamp(x, 0.0, 1.0);
	return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

float smootherstep_range(float edge0, float edge1, float x) {
	return smootherstep01((x - edge0) / max(edge1 - edge0, 1e-9));
}

vec3 direction_for_basis_offset(vec3 center, vec3 right, vec3 up,
		vec2 offset_m) {
	float arc = length(offset_m);
	if (arc < 1e-5) {
		return normalize(center);
	}
	if (arc <= TANGENT_APPROX_MAX_ARC_M) {
		return normalize(center
			+ right * (offset_m.x / u_planet_radius)
			+ up * (offset_m.y / u_planet_radius));
	}
	float raw_theta = arc / max(u_planet_radius, 1.0);
	float theta = min(raw_theta, min(u_visible_cap_angle * 1.02, MAX_PROJECTED_THETA));
	vec3 tangent = normalize(right * offset_m.x + up * offset_m.y);
	return normalize(center * cos(theta) + tangent * sin(theta));
}

vec3 direction_for_offset(vec2 offset_m) {
	return direction_for_basis_offset(u_center_dir, u_center_right, u_center_up,
		offset_m);
}

vec3 stable_surface_offset(vec3 center, vec3 right, vec3 up, vec2 offset_m,
		float altitude_m) {
	float radius = max(u_planet_radius, 1.0);
	float arc = length(offset_m);
	if (arc < 1e-5) {
		return center * altitude_m;
	}

	if (arc <= TANGENT_APPROX_MAX_ARC_M) {
		vec3 tangent_m = right * offset_m.x + up * offset_m.y;
		float q2 = dot(tangent_m, tangent_m) / (radius * radius);
		float root = sqrt(1.0 + q2);
		float inv_root = 1.0 / root;
		float inv_root_minus_one = -q2 / (root * (root + 1.0));
		float radial_m = radius * inv_root_minus_one + altitude_m * inv_root;
		float tangent_scale = (1.0 + altitude_m / radius) * inv_root;
		return center * radial_m + tangent_m * tangent_scale;
	}

	float raw_theta = arc / radius;
	float theta = min(raw_theta, min(u_visible_cap_angle * 1.02, MAX_PROJECTED_THETA));
	vec3 tangent = normalize(right * offset_m.x + up * offset_m.y);
	float sin_theta = sin(theta);
	float cos_theta = cos(theta);
	float half_sin = sin(theta * 0.5);
	float radial_m = -2.0 * radius * half_sin * half_sin
		+ altitude_m * cos_theta;
	float tangent_m = (radius + altitude_m) * sin_theta;
	return center * radial_m + tangent * tangent_m;
}

vec3 macro_array_coord(vec3 dir) {
	vec3 fuv = surface_face_uv(dir);
	ivec3 size3 = textureSize(u_macro_elevation, 0);
	float texture_res = float(size3.x);
	float gutter = max((texture_res - u_macro_face_res) * 0.5, 0.0);
	vec2 uv = (fuv.xy * u_macro_face_res + vec2(gutter)) / texture_res;
	return vec3(uv, fuv.z);
}

float macro_lod_for_spacing(float spacing_m) {
	float macro_texel_m = (PI * 0.5 * u_planet_radius) / max(u_macro_face_res, 1.0);
	float footprint_m = max(spacing_m * MACRO_FILTER_FOOTPRINT_VERTICES, macro_texel_m);
	return clamp(log2(footprint_m / macro_texel_m), 0.0, MACRO_MAX_LOD);
}

vec4 cubic_bspline_weights(float t) {
	float t2 = t * t;
	float t3 = t2 * t;
	return vec4(
		(1.0 - 3.0 * t + 3.0 * t2 - t3) / 6.0,
		(4.0 - 6.0 * t2 + 3.0 * t3) / 6.0,
		(1.0 + 3.0 * t + 3.0 * t2 - 3.0 * t3) / 6.0,
		t3 / 6.0);
}

float macro_bicubic_mip(vec3 coord, int mip) {
	ivec3 size3 = textureSize(u_macro_elevation, mip);
	vec2 size2 = vec2(size3.xy);
	vec2 p = coord.xy * size2 - vec2(0.5);
	vec2 base = floor(p);
	vec2 f = fract(p);

	vec4 wx = cubic_bspline_weights(f.x);
	vec4 wy = cubic_bspline_weights(f.y);
	float gx0 = wx.x + wx.y;
	float gx1 = wx.z + wx.w;
	float gy0 = wy.x + wy.y;
	float gy1 = wy.z + wy.w;

	float x0 = base.x - 1.0 + wx.y / max(gx0, 1e-9);
	float x1 = base.x + 1.0 + wx.w / max(gx1, 1e-9);
	float y0 = base.y - 1.0 + wy.y / max(gy0, 1e-9);
	float y1 = base.y + 1.0 + wy.w / max(gy1, 1e-9);

	vec2 uv00 = (vec2(x0, y0) + vec2(0.5)) / size2;
	vec2 uv10 = (vec2(x1, y0) + vec2(0.5)) / size2;
	vec2 uv01 = (vec2(x0, y1) + vec2(0.5)) / size2;
	vec2 uv11 = (vec2(x1, y1) + vec2(0.5)) / size2;
	float fmip = float(mip);

	float s00 = textureLod(u_macro_elevation, vec3(uv00, coord.z), fmip).r;
	float s10 = textureLod(u_macro_elevation, vec3(uv10, coord.z), fmip).r;
	float s01 = textureLod(u_macro_elevation, vec3(uv01, coord.z), fmip).r;
	float s11 = textureLod(u_macro_elevation, vec3(uv11, coord.z), fmip).r;

	float row0 = mix(s00, s10, gx1);
	float row1 = mix(s01, s11, gx1);
	return mix(row0, row1, gy1);
}

float macro_cubic_trilinear(vec3 coord, float lod) {
	int mip0 = clamp(int(floor(lod)), 0, MACRO_MAX_MIP);
	int mip1 = min(mip0 + 1, MACRO_MAX_MIP);
	float t = fract(lod);
	float h0 = macro_bicubic_mip(coord, mip0);
	if (mip0 == mip1 || t <= 0.001) {
		return h0;
	}
	float h1 = macro_bicubic_mip(coord, mip1);
	return mix(h0, h1, smootherstep01(t));
}

float macro_height(vec3 d, int level) {
	if (u_macro_ready < 0.5 || u_macro_face_res <= 0.5) {
		return 0.0;
	}
	float lod = macro_lod_for_spacing(ring_spacing(level));
	return macro_cubic_trilinear(macro_array_coord(d), lod);
}

float procedural_detail(vec3 d, int level, float macro_h) {
	float spacing = ring_spacing(level);
	uint seed = uint(max(u_detail_seed, 1));
	float coast_guard = mix(0.38, 1.0,
		smootherstep_range(45.0, 320.0, abs(macro_h)));
	return gm_geomorph_height(d, u_planet_radius, spacing, macro_h, seed)
		* coast_guard * u_detail_strength;
}

float terrain_height_for_level(vec3 d, int level) {
	float macro_h = macro_height(d, level);
	return macro_h + procedural_detail(d, level, macro_h);
}

float fine_morph_weight(vec2 cell) {
	float half_cells = u_grid_cells * 0.5;
	float q = length(cell) / max(half_cells, 1.0);
	return smootherstep_range(FINE_MORPH_START_Q, 1.0, q);
}

float coarse_sink_weight(vec2 cell, int level, float is_ring) {
	// Promoted centre discs are never sunk: only annular instances overlap a
	// finer neighbour. This is one of the current 0.0.5 clipmap invariants.
	if (is_ring < 0.5 || level <= 0) {
		return 0.0;
	}
	float half_cells = u_grid_cells * 0.5;
	float q = length(cell) / max(half_cells, 1.0);
	return 1.0 - smootherstep_range(RING_INNER_Q, RING_HANDOFF_Q, q);
}

float terrain_height_with_morph(vec3 d, int level, float morph) {
	float fine = terrain_height_for_level(d, level);
	if (level < ASTERRA_MAX_LEVEL && morph > 0.001) {
		float coarse = terrain_height_for_level(d, level + 1);
		fine = mix(fine, coarse, morph);
	}
	return fine;
}

float macro_height_with_morph(vec3 d, int level, float morph) {
	float fine = macro_height(d, level);
	if (level < ASTERRA_MAX_LEVEL && morph > 0.001) {
		float coarse = macro_height(d, level + 1);
		fine = mix(fine, coarse, morph);
	}
	return fine;
}

vec3 geodesic_offset(vec3 d, vec3 tangent, float distance_m) {
	float theta = distance_m / max(u_planet_radius, 1.0);
	return normalize(d * cos(theta) + tangent * sin(theta));
}

vec3 smooth_macro_normal(vec3 d, int level, float morph, float center_macro_h) {
	vec3 reference_axis = abs(d.y) < 0.95
		? vec3(0.0, 1.0, 0.0)
		: vec3(1.0, 0.0, 0.0);
	vec3 tangent_x = normalize(cross(reference_axis, d));
	vec3 tangent_y = normalize(cross(d, tangent_x));
	float sample_m = max(NORMAL_SAMPLE_MIN_M,
		ring_spacing(level) * NORMAL_SAMPLE_SPACING_MULT);

	vec3 dx = geodesic_offset(d, tangent_x, sample_m);
	vec3 dy = geodesic_offset(d, tangent_y, sample_m);
	float hx = macro_height_with_morph(dx, level, morph);
	float hy = macro_height_with_morph(dy, level, morph);

	vec3 p0 = d * (u_planet_radius + center_macro_h);
	vec3 px = dx * (u_planet_radius + hx);
	vec3 py = dy * (u_planet_radius + hy);
	vec3 n_raw = cross(px - p0, py - p0);
	vec3 n = length(n_raw) > 1e-8 ? normalize(n_raw) : d;
	if (dot(n, d) < 0.0) {
		n = -n;
	}
	return n;
}

void vertex() {
	vec2 cell = UV - vec2(u_grid_cells * 0.5);
	int level = int(floor(INSTANCE_CUSTOM.x + 0.5));
	float is_ring = INSTANCE_CUSTOM.z;
	float spacing = ring_spacing(level);
	vec2 cell_offset_m = cell * spacing;
	float raw_theta = length(cell_offset_m) / max(u_planet_radius, 1.0);
	bool stable_displacement = u_stable_displacement > 0.5;
	// Every active logical level keeps the latest 0.0.5 stationary lattice.
	vec2 level_center_m = round(u_lattice_center_plane / spacing) * spacing;
	vec2 anchor_offset_m = level_center_m + cell_offset_m;
	vec3 d = stable_displacement
		? direction_for_basis_offset(u_anchor_dir, u_anchor_right, u_anchor_up,
			anchor_offset_m)
		: direction_for_offset(cell_offset_m);
	float morph = fine_morph_weight(cell);
	float macro_h = macro_height_with_morph(d, level, morph);
	float h = terrain_height_with_morph(d, level, morph);
	float sink = coarse_sink_weight(cell, level, is_ring) * max(spacing * u_sink_scale, 0.5);
	float height = (u_height_enabled > 0.5 ? h : 0.0) - sink;
	float altitude_m = height + u_surface_bias;
	vec3 p = d * (u_planet_radius + altitude_m);

	v_planet = p;
	v_render = stable_displacement
		? u_anchor_render + stable_surface_offset(u_anchor_dir, u_anchor_right,
			u_anchor_up, anchor_offset_m, altitude_m)
		: p - u_origin;
	v_smooth_normal = u_height_enabled > 0.5
		? smooth_macro_normal(d, level, morph, macro_h)
		: d;
	v_valid = u_macro_ready;
	v_cap_valid = raw_theta <= u_visible_cap_angle ? 1.0 : 0.0;
	v_level = float(level);

	vec3 sun_dir = normalize(u_cloud_shadow_sun_dir);
	float planet_vis = planet_direct_sun_visibility(p, sun_dir, u_planet_radius);
	v_direct_transmittance = planet_vis;
	if (planet_vis > 0.001) {
		v_direct_transmittance *= cloud_shadow_transmittance(
			p, sun_dir, u_planet_radius);
	}
	VERTEX = v_render;
}

void fragment() {
	if (v_valid < 0.5 || v_cap_valid < 0.5) {
		discard;
	}
	if (u_debug_side_cut > 0.5) {
		vec3 planet_dir = normalize(v_planet);
		if (dot(planet_dir, u_center_up) > 0.0) {
			discard;
		}
	}

	vec3 up = normalize(v_planet);
	vec3 wn = normalize(v_smooth_normal);
	if (dot(wn, up) < 0.0) {
		wn = -wn;
	}
	NORMAL = normalize((VIEW_MATRIX * vec4(wn, 0.0)).xyz);

	if (u_debug_side_cut > 0.5) {
		float phase = v_level * 0.13;
		vec3 lod_color = 0.5 + 0.5 * cos(TAU * (vec3(phase) + vec3(0.0, 0.33, 0.67)));
		ALBEDO = mix(vec3(0.06), lod_color, 0.92);
		EMISSION = lod_color * 0.10;
		ROUGHNESS = 1.0;
		SPECULAR = 0.0;
	} else {
		float slope = clamp(1.0 - dot(wn, up), 0.0, 1.0);
		vec4 surf = vec4(0.38, 0.38, 0.12, 0.0);
		// Preserve the latest resident whole-planet material texture as the
		// surface fallback while geometry integration is validated.
		surf = material_clipmap_surface(v_planet, u_planet_radius, surf);
		vec3 soil = vec3(0.24, 0.17, 0.105);
		vec3 grass = vec3(0.17, 0.255, 0.095);
		vec3 wet = vec3(0.12, 0.145, 0.085);
		vec3 sand = vec3(0.48, 0.39, 0.23);
		vec3 rock = vec3(0.285, 0.275, 0.255);
		vec3 snow = vec3(0.78, 0.80, 0.81);
		vec3 albedo = mix(soil, grass, clamp(surf.r, 0.0, 1.0));
		albedo = mix(albedo, wet, clamp(surf.g * 0.70, 0.0, 1.0));
		albedo = mix(albedo, sand, clamp(surf.b, 0.0, 1.0));
		albedo = mix(albedo, snow, clamp(surf.a, 0.0, 1.0));
		albedo = mix(albedo, rock, smoothstep(0.08, 0.34, slope));
		ALBEDO = albedo;
		ROUGHNESS = mix(0.94, 0.72, smoothstep(0.10, 0.42, slope));
		SPECULAR = 0.22;
	}
}

void light() {
	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	if (ndotl > 0.0) {
		float visibility = LIGHT_IS_DIRECTIONAL ? clamp(v_direct_transmittance, 0.0, 1.0) : 1.0;
		vec3 radiance = LIGHT_COLOR * ATTENUATION * visibility;

		float ndotv = max(dot(NORMAL, VIEW), 0.001);
		vec3 h = normalize(LIGHT + VIEW);
		float ndoth = max(dot(NORMAL, h), 0.0);
		float vdoth = max(dot(VIEW, h), 0.0);
		float rough = clamp(ROUGHNESS, 0.045, 1.0);

		float fd90 = 0.5 + 2.0 * vdoth * vdoth * rough;
		float fl = pow(1.0 - ndotl, 5.0);
		float fv = pow(1.0 - ndotv, 5.0);
		float diffuse_term = (1.0 + (fd90 - 1.0) * fl)
			* (1.0 + (fd90 - 1.0) * fv);
		DIFFUSE_LIGHT += radiance * ndotl * diffuse_term / PI;

		if (u_debug_side_cut <= 0.5) {
			float alpha = rough * rough;
			float a2 = alpha * alpha;
			float denom = ndoth * ndoth * (a2 - 1.0) + 1.0;
			float d = a2 / max(PI * denom * denom, 1e-5);
			float k = (rough + 1.0) * (rough + 1.0) * 0.125;
			float gv = ndotv / max(ndotv * (1.0 - k) + k, 1e-4);
			float gl = ndotl / max(ndotl * (1.0 - k) + k, 1e-4);
			float f0 = 0.018;
			float fresnel = f0 + (1.0 - f0) * pow(1.0 - vdoth, 5.0);
			float spec = d * gv * gl * fresnel
				/ max(4.0 * ndotv * ndotl, 1e-4);
			SPECULAR_LIGHT += radiance * ndotl * spec * SPECULAR_AMOUNT;
		}
	}
}
