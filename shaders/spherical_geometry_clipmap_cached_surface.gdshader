shader_type spatial;
render_mode cull_front, diffuse_burley, specular_schlick_ggx, ambient_light_disabled;

#include "res://shaders/material_clipmap.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/terrain_sun_occlusion.gdshaderinc"
#include "res://shaders/gpu_planet_context.gdshaderinc"
#include "res://shaders/gpu_geomorph.gdshaderinc"
#include "res://shaders/gpu_surface_antitile.gdshaderinc"
#include "res://shaders/gpu_surface_classifier.gdshaderinc"
#include "res://shaders/gpu_material_microrelief.gdshaderinc"
#include "res://shaders/gpu_rock_pbr.gdshaderinc"
#include "res://shaders/gpu_surface_pbr.gdshaderinc"
#include "res://shaders/planet_lighting.gdshaderinc"
#include "res://shaders/cloud_shadow.gdshaderinc"
#include "res://shaders/terrain_aerial_gpu.gdshaderinc"
#include "res://shaders/terrain_edit_delta.gdshaderinc"

uniform sampler2DArray u_macro_elevation : filter_linear_mipmap, repeat_disable;
uniform float u_macro_ready = 0.0;
uniform float u_macro_face_res = 0.0;
uniform float u_terrain_sun_occlusion : hint_range(0.0, 1.0) = 1.0;
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
uniform int u_debug_geomorph_mode = 0;

varying vec3 v_planet;
varying vec3 v_render;
varying vec3 v_smooth_normal;
varying vec3 v_atmo_t;
varying vec3 v_atmo_l;
varying float v_valid;
varying float v_cap_valid;
varying float v_level;
varying float v_macro_h;
varying float v_direct_transmittance;
varying float v_micro_layer;
varying float v_geom_normal_weight;

const int ASTERRA_MAX_LEVEL = 14;
const float RING_INNER_Q = 0.44;
const float RING_HANDOFF_Q = 0.50;
const float FINE_MORPH_START_Q = 0.875;
const float MAX_PROJECTED_THETA = 1.62;
const float TANGENT_FAST_MAX_ARC_M = 70000.0;
const float PROJECTION_BLEND_END_M = 110000.0;
const float MACRO_FILTER_FOOTPRINT_VERTICES = 3.0;
const float MACRO_MAX_LOD = 5.0;
const int MACRO_MAX_MIP = 5;
const float NORMAL_SAMPLE_SPACING_MULT = 2.0;
const float NORMAL_SAMPLE_MIN_M = 1.5;

// The coarser annulus is only a hidden overlap surface. Keep it a few millimetres
// below the exact child-morphed surface to order depth without opening a visible gap.
const float RING_HANDOFF_BIAS_MIN_M = 0.004;
const float RING_HANDOFF_BIAS_MAX_M = 0.025;
const float RING_HANDOFF_BIAS_SPACING = 0.001;

const float MICRO_HANDOFF_START_L0_CELLS = 24.0;
const float MICRO_OUTER_L0_CELLS = 32.0;
const float MICRO_L0_SINK_M = 0.30;
const float MICRO_DETAIL_NEAR_M = 90.0;
const float MICRO_DETAIL_FAR_M = 160.0;

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

float projected_theta(float arc, float radius) {
	float q = arc / max(radius, 1.0);
	float gnomonic_theta = atan(q);
	float geodesic_theta = q;
	float blend = smootherstep_range(TANGENT_FAST_MAX_ARC_M, PROJECTION_BLEND_END_M, arc);
	return min(mix(gnomonic_theta, geodesic_theta, blend), MAX_PROJECTED_THETA);
}

vec3 direction_for_basis_offset(vec3 center, vec3 right, vec3 up, vec2 offset_m) {
	float arc = length(offset_m);
	if (arc < 1e-5) return normalize(center);
	if (arc <= TANGENT_FAST_MAX_ARC_M) {
		return normalize(center + right * (offset_m.x / u_planet_radius)
			+ up * (offset_m.y / u_planet_radius));
	}
	float theta = min(projected_theta(arc, u_planet_radius), u_visible_cap_angle * 1.02);
	vec3 tangent = normalize(right * offset_m.x + up * offset_m.y);
	return normalize(center * cos(theta) + tangent * sin(theta));
}

vec3 direction_for_offset(vec2 offset_m) {
	return direction_for_basis_offset(u_center_dir, u_center_right, u_center_up, offset_m);
}

vec3 stable_surface_offset(vec3 center, vec3 right, vec3 up, vec2 offset_m,
		float altitude_m) {
	float radius = max(u_planet_radius, 1.0);
	float arc = length(offset_m);
	if (arc < 1e-5) return center * altitude_m;
	if (arc <= TANGENT_FAST_MAX_ARC_M) {
		vec3 tangent_m = right * offset_m.x + up * offset_m.y;
		float q2 = dot(tangent_m, tangent_m) / (radius * radius);
		float root = sqrt(1.0 + q2);
		float inv_root = 1.0 / root;
		float inv_root_minus_one = -q2 / (root * (root + 1.0));
		float radial_m = radius * inv_root_minus_one + altitude_m * inv_root;
		float tangent_scale = (1.0 + altitude_m / radius) * inv_root;
		return center * radial_m + tangent_m * tangent_scale;
	}
	float theta = min(projected_theta(arc, radius), u_visible_cap_angle * 1.02);
	vec3 tangent = normalize(right * offset_m.x + up * offset_m.y);
	float sin_theta = sin(theta);
	float cos_theta = cos(theta);
	float half_sin = sin(theta * 0.5);
	float radial_m = -2.0 * radius * half_sin * half_sin + altitude_m * cos_theta;
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
	return vec4((1.0 - 3.0 * t + 3.0 * t2 - t3) / 6.0,
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
	return mix(mix(s00, s10, gx1), mix(s01, s11, gx1), gy1);
}

float macro_cubic_trilinear(vec3 coord, float lod) {
	int mip0 = clamp(int(floor(lod)), 0, MACRO_MAX_MIP);
	int mip1 = min(mip0 + 1, MACRO_MAX_MIP);
	float t = fract(lod);
	float h0 = macro_bicubic_mip(coord, mip0);
	if (mip0 == mip1 || t <= 0.001) return h0;
	return mix(h0, macro_bicubic_mip(coord, mip1), smootherstep01(t));
}

float macro_height(vec3 d, int level) {
	if (u_macro_ready < 0.5 || u_macro_face_res <= 0.5) return 0.0;
	return macro_cubic_trilinear(macro_array_coord(d), macro_lod_for_spacing(ring_spacing(level)));
}

float procedural_detail(vec3 d, int level, float macro_h) {
	float spacing = ring_spacing(level);
	uint seed = uint(max(u_detail_seed, 1));
	float coast_guard = mix(0.38, 1.0, smootherstep_range(45.0, 320.0, abs(macro_h)));
	return gm_geomorph_height(d, u_planet_radius, spacing, macro_h, seed)
		* coast_guard * u_detail_strength;
}

vec2 terrain_sample_for_level(vec3 d, int level) {
	float macro_h = macro_height(d, level);
	return vec2(macro_h, macro_h + procedural_detail(d, level, macro_h));
}

float fine_morph_weight(vec2 cell) {
	float half_cells = u_grid_cells * 0.5;
	return smootherstep_range(FINE_MORPH_START_Q, 1.0,
		length(cell) / max(half_cells, 1.0));
}

float ring_handoff_weight(vec2 cell, int level, float is_ring) {
	if (is_ring < 0.5 || level <= 0) return 0.0;
	float half_cells = u_grid_cells * 0.5;
	float q = length(cell) / max(half_cells, 1.0);
	return 1.0 - smootherstep_range(RING_INNER_Q, RING_HANDOFF_Q, q);
}

#include "res://shaders/terrain_clipmap_cache_sampling.gdshaderinc"

vec3 terrain_debug_color(int mode, vec4 landform, vec4 primary, vec4 secondary,
		vec4 soil, vec4 geology, vec4 hydro, float rock_id, float biome) {
	if (mode == 1) return clamp(landform.rgb + vec3(landform.a * 0.55), 0.0, 1.0);
	if (mode == 2) return clamp(vec3(primary.r + primary.a,
		primary.g + primary.a * 0.78 + primary.b * 0.45, primary.b), 0.0, 1.0);
	if (mode == 3) return clamp(vec3(0.62, 0.22, 0.08) * secondary.r
		+ vec3(1.0) * secondary.g + vec3(0.70, 0.62, 0.43) * secondary.b
		+ vec3(0.10, 0.72, 0.86) * secondary.a, 0.0, 1.0);
	if (mode == 4) return clamp(vec3(soil.r, soil.g, soil.b) * (1.0 - soil.a * 0.55), 0.0, 1.0);
	if (mode == 5) return vec3(rock_id / 12.0, biome / 19.0, geology.r);
	if (mode == 6) return clamp(vec3(hydro.rg, hydro.b) + vec3(hydro.a * 0.32), 0.0, 1.0);
	return vec3(0.0);
}

// The map inspector intentionally uses a categorical, high-chroma biome palette.
// For terrain rendering we reconstruct that same palette spatially instead of
// interpolating biome numbers. Four categorical texels are converted to colours
// first, then blended with a controllable sigmoid. This gives a true smooth fade
// at ecological borders without turning an entire coarse context cell into a
// broad pastel band when viewed from orbit.
vec3 terrain_biome_map_color(float biome) {
	if (biome < 0.5) return vec3(0.05, 0.13, 0.33);
	if (biome < 1.5) return vec3(0.10, 0.30, 0.50);
	if (biome < 2.5) return vec3(0.93, 0.96, 1.00);
	if (biome < 3.5) return vec3(0.62, 0.66, 0.58);
	if (biome < 4.5) return vec3(0.16, 0.33, 0.22);
	if (biome < 5.5) return vec3(0.62, 0.60, 0.52);
	if (biome < 6.5) return vec3(0.62, 0.68, 0.34);
	if (biome < 7.5) return vec3(0.20, 0.44, 0.20);
	if (biome < 8.5) return vec3(0.12, 0.40, 0.26);
	if (biome < 9.5) return vec3(0.55, 0.58, 0.26);
	if (biome < 10.5) return vec3(0.68, 0.63, 0.36);
	if (biome < 11.5) return vec3(0.83, 0.73, 0.47);
	if (biome < 12.5) return vec3(0.72, 0.66, 0.28);
	if (biome < 13.5) return vec3(0.32, 0.50, 0.20);
	if (biome < 14.5) return vec3(0.10, 0.36, 0.16);
	if (biome < 15.5) return vec3(0.28, 0.44, 0.34);
	if (biome < 16.5) return vec3(0.55, 0.55, 0.58);
	return vec3(0.44, 0.42, 0.40);
}

float terrain_biome_edge_weight(float t, float sharpness) {
	t = clamp(t, 0.0, 1.0);
	float a = pow(max(t, 1e-5), sharpness);
	float b = pow(max(1.0 - t, 1e-5), sharpness);
	return a / max(a + b, 1e-6);
}

float terrain_biome_id_texel(ivec2 texel, int layer, ivec2 size2) {
	ivec2 p = clamp(texel, ivec2(0), size2 - ivec2(1));
	return floor(texelFetch(u_ctx_biome, ivec3(p, layer), 0).r * 19.0 + 0.5);
}

vec3 terrain_biome_palette(vec3 dir, float sharpness) {
	if (u_ctx_ready < 0.5 || u_ctx_face_res <= 0.5) {
		return terrain_biome_map_color(7.0);
	}
	vec3 coord = asterra_context_coord(dir);
	ivec3 size3 = textureSize(u_ctx_biome, 0);
	vec2 p = coord.xy * vec2(size3.xy) - vec2(0.5);
	ivec2 base = ivec2(floor(p));
	vec2 f = fract(p);
	f = vec2(terrain_biome_edge_weight(f.x, sharpness),
		terrain_biome_edge_weight(f.y, sharpness));
	int layer = int(coord.z + 0.5);
	float id00 = terrain_biome_id_texel(base, layer, size3.xy);
	float id10 = terrain_biome_id_texel(base + ivec2(1, 0), layer, size3.xy);
	float id01 = terrain_biome_id_texel(base + ivec2(0, 1), layer, size3.xy);
	float id11 = terrain_biome_id_texel(base + ivec2(1, 1), layer, size3.xy);
	vec3 c00 = terrain_biome_map_color(id00);
	vec3 c10 = terrain_biome_map_color(id10);
	vec3 c01 = terrain_biome_map_color(id01);
	vec3 c11 = terrain_biome_map_color(id11);
	return mix(mix(c00, c10, f.x), mix(c01, c11, f.x), f.y);
}

void vertex() {
	vec2 cell = UV - vec2(u_grid_cells * 0.5);
	float cell_radius = length(cell);
	int level = int(floor(INSTANCE_CUSTOM.x + 0.5));
	float is_ring = INSTANCE_CUSTOM.z;
	float micro_layer = INSTANCE_CUSTOM.w;
	float spacing = ring_spacing(level);
	vec2 cell_offset_m = cell * spacing;
	float raw_theta = length(cell_offset_m) / max(u_planet_radius, 1.0);
	bool stable_displacement = u_stable_displacement > 0.5;
	vec2 level_center_m = round(u_lattice_center_plane / spacing) * spacing;
	vec2 anchor_offset_m = level_center_m + cell_offset_m;
	vec3 d = stable_displacement
		? direction_for_basis_offset(u_anchor_dir, u_anchor_right, u_anchor_up, anchor_offset_m)
		: direction_for_offset(cell_offset_m);

	float morph = fine_morph_weight(cell);
	vec2 sample = terrain_cached_sample_with_morph(
		d, level, morph, anchor_offset_m, stable_displacement);
	float macro_h = sample.x;
	float h = sample.y;
	float handoff_w = ring_handoff_weight(cell, level, is_ring);
	int normal_level = level;
	float normal_morph = morph;

	if (handoff_w > 0.001 && level > 0) {
		int child_level = level - 1;
		float child_spacing = ring_spacing(child_level);
		vec2 child_cell;
		if (stable_displacement) {
			vec2 child_center_m = round(u_lattice_center_plane / child_spacing) * child_spacing;
			child_cell = (anchor_offset_m - child_center_m) / child_spacing;
		} else {
			child_cell = cell_offset_m / child_spacing;
		}
		float child_morph = fine_morph_weight(child_cell);
		vec2 child_sample = terrain_cached_sample_with_morph(
			d, child_level, child_morph, anchor_offset_m, stable_displacement);
		// The child already morphs continuously to this parent over exactly the
		// shared annulus. A second independent parent->child blend produces two
		// different overlapping height sheets (with either sign depending on relief).
		// Clone the child's final morphed height instead; at q=0.5 child_morph=1, so
		// this naturally converges back to the pure parent with no discontinuity.
		macro_h = child_sample.x;
		h = child_sample.y;
		normal_level = child_level;
		normal_morph = child_morph;
	}

	float edit_h = terrain_edit_delta(d, u_planet_radius);
	h += edit_h;

	float handoff_bias_m = clamp(spacing * RING_HANDOFF_BIAS_SPACING * max(u_sink_scale, 0.0),
		RING_HANDOFF_BIAS_MIN_M, RING_HANDOFF_BIAS_MAX_M) * handoff_w;
	float sink = handoff_bias_m;

	if (micro_layer < 0.5 && is_ring < 0.5 && level == 0
			&& cell_radius < MICRO_OUTER_L0_CELLS) {
		float overlap_sink = 1.0 - smootherstep_range(
			MICRO_HANDOFF_START_L0_CELLS, MICRO_OUTER_L0_CELLS, cell_radius);
		sink += overlap_sink * MICRO_L0_SINK_M;
	}

	float height = (u_height_enabled > 0.5 ? h : 0.0) - sink;
	float altitude_m = height + u_surface_bias;
	vec3 p = d * (u_planet_radius + altitude_m);
	vec3 render_p = stable_displacement
		? u_anchor_render + stable_surface_offset(u_anchor_dir, u_anchor_right,
			u_anchor_up, anchor_offset_m, altitude_m)
		: p - u_origin;
	float vertex_camera_dist = length(render_p - INV_VIEW_MATRIX[3].xyz);

	if (micro_layer > 0.5 && level == 0 && u_height_enabled > 0.5) {
		float edge_w = 1.0 - smootherstep_range(
			MICRO_HANDOFF_START_L0_CELLS, MICRO_OUTER_L0_CELLS, cell_radius);
		float proximity_w = 1.0 - smoothstep(MICRO_DETAIL_NEAR_M, MICRO_DETAIL_FAR_M, vertex_camera_dist);
		float micro_w = edge_w * proximity_w;
		if (micro_w > 0.001) {
			vec4 soil = asterra_ctx_soil(d);
			vec4 surface = asterra_ctx_surface(d);
			vec4 geology = asterra_ctx_geology(d);
			vec4 structure = asterra_ctx_structure(d);
			vec4 climate = asterra_ctx_climate(d);
			vec4 hydro = asterra_ctx_hydrology(d);
			float rock_id = asterra_ctx_rock_id(d);
			float biome = asterra_ctx_biome_id(d);
			vec4 landform = gm_landform_weights_from_context(
				macro_h, soil, surface, geology, structure, climate, hydro);
			float micro_h = mmr_height(render_p + u_detail_origin, rock_id, biome,
				soil, surface, geology, structure, climate, hydro, landform);
			height += micro_h * micro_w;
			altitude_m = height + u_surface_bias;
			p = d * (u_planet_radius + altitude_m);
			render_p = stable_displacement
				? u_anchor_render + stable_surface_offset(u_anchor_dir, u_anchor_right,
					u_anchor_up, anchor_offset_m, altitude_m)
				: p - u_origin;
		}
	}

	if (u_height_enabled > 0.5 && stable_displacement) {
		v_smooth_normal = terrain_cached_surface_normal(
			d, normal_level, normal_morph, h, anchor_offset_m);
		v_geom_normal_weight = max(normal_level <= 1 ? 0.28 : 0.10,
			abs(edit_h) > 0.001 ? 0.90 : 0.0);
	} else {
		v_smooth_normal = d;
		v_geom_normal_weight = 0.65;
	}

	v_planet = p;
	v_render = render_p;
	v_valid = u_macro_ready;
	v_cap_valid = raw_theta <= u_visible_cap_angle ? 1.0 : 0.0;
	v_level = float(level);
	v_macro_h = macro_h;
	v_micro_layer = micro_layer;

	vec3 sun_dir = normalize(u_cloud_shadow_sun_dir);
	float planet_vis = planet_direct_sun_visibility(p, sun_dir, u_planet_radius);
	v_direct_transmittance = planet_vis;
	if (planet_vis > 0.001) {
		if (u_height_enabled > 0.5 && u_macro_ready > 0.5
				&& u_macro_face_res > 0.5 && u_terrain_sun_occlusion > 0.001) {
			int terrain_shadow_steps = level <= 2 ? 12 : (level <= 5 ? 9 : 6);
			float terrain_vis = terrain_sun_visibility(
				u_macro_elevation, p, sun_dir, u_macro_face_res,
				u_planet_radius, terrain_shadow_steps);
			v_direct_transmittance *= mix(1.0, terrain_vis, u_terrain_sun_occlusion);
		}
		v_direct_transmittance *= cloud_shadow_transmittance(p, sun_dir, u_planet_radius);
	}
	vec3 camera_planet = INV_VIEW_MATRIX[3].xyz + u_origin;
	vec3 atmo_t;
	vec3 atmo_l;
	terrain_aerial_perspective(camera_planet, p, sun_dir, u_planet_radius, atmo_t, atmo_l);
	v_atmo_t = atmo_t;
	v_atmo_l = atmo_l;
	VERTEX = v_render;
}

void fragment() {
	if (v_valid < 0.5 || v_cap_valid < 0.5) discard;
	if (u_debug_side_cut > 0.5) {
		vec3 planet_dir = normalize(v_planet);
		if (dot(planet_dir, u_center_up) > 0.0) discard;
	}
	vec3 up = normalize(v_planet);
	vec3 smooth_n = normalize(v_smooth_normal);
	if (dot(smooth_n, up) < 0.0) smooth_n = -smooth_n;
	vec3 geom_raw = cross(dFdx(v_render), dFdy(v_render));
	vec3 geom_n = dot(geom_raw, geom_raw) > 1e-12 ? normalize(geom_raw) : smooth_n;
	if (dot(geom_n, up) < 0.0) geom_n = -geom_n;
	float geom_shading_w = max(
		(1.0 - smoothstep(2.0, 5.0, v_level)) * mix(0.04, 0.18, v_micro_layer),
		v_geom_normal_weight);
	vec3 wn = normalize(mix(smooth_n, geom_n, geom_shading_w));
	NORMAL = normalize((VIEW_MATRIX * vec4(wn, 0.0)).xyz);

	if (u_debug_side_cut > 0.5) {
		float phase = v_level * 0.13 + v_micro_layer * 0.08;
		vec3 lod_color = 0.5 + 0.5 * cos(TAU * (vec3(phase) + vec3(0.0, 0.33, 0.67)));
		ALBEDO = mix(vec3(0.06), lod_color, 0.92);
		EMISSION = lod_color * 0.10;
		ROUGHNESS = 1.0;
		SPECULAR = 0.0;
		METALLIC = 0.0;
	} else {
		vec4 soil = asterra_ctx_soil(up);
		vec4 surface = asterra_ctx_surface(up);
		vec4 geology = asterra_ctx_geology(up);
		vec4 structure = asterra_ctx_structure(up);
		vec4 climate = asterra_ctx_climate(up);
		vec4 hydro = asterra_ctx_hydrology(up);
		float rock_id = asterra_ctx_rock_id(up);
		float biome = asterra_ctx_biome_id(up);
		vec4 landform = gm_landform_weights_from_context(v_macro_h,
			soil, surface, geology, structure, climate, hydro);
		float slope_deg = acos(clamp(dot(geom_n, up), 0.0, 1.0)) * 57.2957795131;
		vec4 primary = sc_primary_weights(
			soil, surface, geology, structure, climate, hydro, landform, biome, slope_deg);
		vec4 secondary = sc_secondary_weights(
			soil, surface, geology, structure, climate, hydro, landform, biome, slope_deg);
		primary *= 1.0 - secondary.g * 0.88;
		secondary.r *= 1.0 - secondary.g * 0.92;

		if (u_debug_geomorph_mode > 0) {
			ALBEDO = vec3(0.0);
			EMISSION = terrain_debug_color(u_debug_geomorph_mode,
				landform, primary, secondary, soil, geology, hydro, rock_id, biome);
			ROUGHNESS = 1.0;
			SPECULAR = 0.0;
			METALLIC = 0.0;
		} else {
			vec3 base_albedo = sc_compose_albedo(primary, secondary, soil, surface, climate, biome, rock_id);
			float base_roughness = sc_compose_roughness(primary, secondary, surface);
			float camera_dist = length(v_render - CAMERA_POSITION_WORLD);
			float material_total = dot(primary, vec4(1.0)) + dot(secondary, vec4(1.0));
			float rock_mix_w = material_total > 1e-4 ? primary.r / material_total : 0.0;
			vec3 rock_albedo;
			float rock_roughness;
			float material_specular;
			vec3 rock_n;
			float rock_normal_w;
			rrp_apply(v_render + u_detail_origin, wn, camera_dist,
				rock_id, geology, structure, rock_mix_w,
				base_albedo, base_roughness,
				rock_albedo, rock_roughness, material_specular, rock_n, rock_normal_w);
			if (rock_normal_w > 0.001) {
				rock_normal_w *= mix(1.0, 0.62, v_micro_layer);
				wn = normalize(mix(wn, rock_n, rock_normal_w));
				NORMAL = normalize((VIEW_MATRIX * vec4(wn, 0.0)).xyz);
			}
			vec3 dpos = spbr_antitile_position(v_render + u_detail_origin);
			vec3 albedo;
			float roughness;
			vec3 scan_n;
			float scan_normal_w;
			spbr_apply(dpos, wn, camera_dist, primary, secondary, biome,
				rock_albedo, rock_roughness, albedo, roughness, scan_n, scan_normal_w);
			// Close range remains the physically classified/PBR surface. Once the
			// terrain is seen as landscape rather than individual material, restore
			// some of the biome inspector's chroma and categorical structure. The
			// transition itself spans tens of kilometres, while ecological edges are
			// sharpened independently so orbit does not look Gaussian-blurred.
			float orbit_palette_w = smootherstep_range(12000.0, 90000.0, camera_dist);
			if (orbit_palette_w > 0.001) {
				float edge_sharpness = mix(1.35, 3.20, orbit_palette_w);
				vec3 map_palette = terrain_biome_palette(up, edge_sharpness);
				vec3 palette_anchor = mix(albedo, map_palette, 0.38);
				albedo = mix(albedo, palette_anchor, orbit_palette_w);
			}
			if (scan_normal_w > 0.001) {
				scan_normal_w *= mix(1.0, 0.78, v_micro_layer);
				wn = normalize(mix(wn, scan_n, scan_normal_w));
				NORMAL = normalize((VIEW_MATRIX * vec4(wn, 0.0)).xyz);
			}
			ALBEDO = albedo;
			ROUGHNESS = roughness;
			SPECULAR = material_specular;
			METALLIC = 0.0;
			vec3 sun_dir = normalize(u_cloud_shadow_sun_dir);
			vec3 skylight = planet_sky_irradiance(v_planet, sun_dir, u_planet_radius);
			float sky_facing = clamp(dot(wn, up), 0.0, 1.0);
			float sky_exposure = mix(0.32, 1.0, pow(sky_facing, 0.72));
			EMISSION = albedo * skylight * sky_exposure * v_atmo_t + v_atmo_l;
		}
	}
}

void light() {
	if (u_debug_geomorph_mode <= 0) {
		float ndotl = max(dot(NORMAL, LIGHT), 0.0);
		if (ndotl > 0.0) {
			float visibility = LIGHT_IS_DIRECTIONAL ? clamp(v_direct_transmittance, 0.0, 1.0) : 1.0;
			vec3 radiance = LIGHT_COLOR * ATTENUATION * visibility * v_atmo_t;
			float ndotv = max(dot(NORMAL, VIEW), 0.001);
			vec3 h = normalize(LIGHT + VIEW);
			float ndoth = max(dot(NORMAL, h), 0.0);
			float vdoth = max(dot(VIEW, h), 0.0);
			float rough = clamp(ROUGHNESS, 0.045, 1.0);
			float fd90 = 0.5 + 2.0 * vdoth * vdoth * rough;
			float fl = pow(1.0 - ndotl, 5.0);
			float fv = pow(1.0 - ndotv, 5.0);
			float diffuse_term = (1.0 + (fd90 - 1.0) * fl) * (1.0 + (fd90 - 1.0) * fv);
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
				float spec = d * gv * gl * fresnel / max(4.0 * ndotv * ndotl, 1e-4);
				SPECULAR_LIGHT += radiance * ndotl * spec * SPECULAR_AMOUNT;
			}
		}
	}
}
