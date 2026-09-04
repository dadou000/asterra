shader_type spatial;
render_mode cull_front, diffuse_burley, specular_schlick_ggx;

uniform float u_planet_radius = 1000000.0;
uniform vec3 u_origin = vec3(0.0);
uniform vec3 u_anchor_render = vec3(0.0);
uniform vec3 u_anchor_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_anchor_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_anchor_up = vec3(0.0, 1.0, 0.0);
uniform vec2 u_lattice_center_plane = vec2(0.0);
uniform float u_base_spacing = 0.75;
uniform float u_grid_cells = 400.0;
uniform float u_visible_cap_angle = 1.5707963;
uniform float u_surface_bias = 0.035;
uniform float u_sink_scale = 2.0;
uniform float u_generated_height_enabled = 0.0;
uniform float u_shader_displacement_enabled = 1.0;
uniform float u_height_enabled = 0.0;
uniform float u_debug_side_cut = 0.0;

uniform sampler2D u_author_biome_override : filter_nearest, repeat_disable;
uniform float u_author_biome_ready = 0.0;
uniform vec3 u_author_biome_center = vec3(1.0, 0.0, 0.0);
uniform vec3 u_author_biome_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_author_biome_up = vec3(0.0, 1.0, 0.0);
uniform float u_author_biome_half_extent_m = 512.0;
uniform float u_author_biome_planet_radius = 1000000.0;

varying vec3 v_planet;
varying vec3 v_render;
varying float v_biome;
varying float v_cap_valid;
varying float v_level;
varying float v_micro_layer;

const float TANGENT_FAST_MAX_ARC_M = 70000.0;
const float PROJECTION_BLEND_END_M = 110000.0;
const float MAX_PROJECTED_THETA = 1.62;
const float MICRO_HANDOFF_START_L0_CELLS = 24.0;
const float MICRO_OUTER_L0_CELLS = 32.0;
const float MICRO_L0_SINK_M = 0.30;

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

vec3 stable_surface_offset(vec3 center, vec3 right, vec3 up, vec2 offset_m, float altitude_m) {
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

float authored_biome_id(vec3 direction) {
	if (u_author_biome_ready < 0.5) return 0.0;
	float denom = dot(direction, u_author_biome_center);
	if (denom <= 0.01) return 0.0;
	float radius = max(u_author_biome_planet_radius, 1.0);
	vec2 plane = vec2(dot(direction, u_author_biome_right), dot(direction, u_author_biome_up))
		* (radius / denom);
	float extent = max(u_author_biome_half_extent_m, 0.001);
	if (abs(plane.x) > extent || abs(plane.y) > extent) return 0.0;
	vec2 uv = vec2(plane.x / (extent * 2.0) + 0.5, 0.5 - plane.y / (extent * 2.0));
	vec2 sample_value = textureLod(u_author_biome_override, uv, 0.0).rg;
	return sample_value.g > 0.5 ? clamp(round(sample_value.r * 19.0), 0.0, 17.0) : 0.0;
}

float _authored_texel_membership(ivec2 texel, int biome) {
	vec4 t = texelFetch(u_author_biome_override, texel, 0);
	int id = t.g > 0.5 ? int(clamp(round(t.r * 19.0), 0.0, 17.0)) : 0;
	return (id == biome) ? 1.0 : 0.0;
}

// Bilinearly-filtered 0..1 membership of `biome` -- a C0-continuous ramp about one
// override texel wide instead of the categorical id's hard step.
float authored_biome_membership(vec3 direction, int biome) {
	if (u_author_biome_ready < 0.5) {
		return (biome == 0) ? 1.0 : 0.0;
	}
	float denom = dot(direction, u_author_biome_center);
	if (denom <= 0.01) {
		return (biome == 0) ? 1.0 : 0.0;
	}
	float radius = max(u_author_biome_planet_radius, 1.0);
	vec2 plane = vec2(dot(direction, u_author_biome_right), dot(direction, u_author_biome_up))
		* (radius / denom);
	float extent = max(u_author_biome_half_extent_m, 0.001);
	if (abs(plane.x) > extent || abs(plane.y) > extent) {
		return (biome == 0) ? 1.0 : 0.0;
	}
	vec2 uv = vec2(plane.x / (extent * 2.0) + 0.5, 0.5 - plane.y / (extent * 2.0));
	ivec2 size2 = textureSize(u_author_biome_override, 0);
	vec2 p = uv * vec2(size2) - vec2(0.5);
	vec2 f = fract(p);
	ivec2 b = ivec2(floor(p));
	ivec2 hi = size2 - ivec2(1);
	float m00 = _authored_texel_membership(clamp(b, ivec2(0), hi), biome);
	float m10 = _authored_texel_membership(clamp(b + ivec2(1, 0), ivec2(0), hi), biome);
	float m01 = _authored_texel_membership(clamp(b + ivec2(0, 1), ivec2(0), hi), biome);
	float m11 = _authored_texel_membership(clamp(b + ivec2(1, 1), ivec2(0), hi), biome);
	return mix(mix(m00, m10, f.x), mix(m01, m11, f.x), f.y);
}

vec3 biome_color(float biome) {
	if (biome < 0.5) return vec3(0.34, 0.35, 0.36);
	float phase = biome * 0.61803398875;
	return 0.34 + 0.24 * cos(6.28318530718 * (vec3(phase) + vec3(0.00, 0.33, 0.67)));
}

#define ASTERRA_BIOME_ID_AT(p) authored_biome_id(p)
#define ASTERRA_BIOME_MEMBERSHIP(p, b) authored_biome_membership(p, b)
#include "res://shaders/terrain_biome_profile.gdshaderinc"

void vertex() {
	int level = clamp(int(round(INSTANCE_CUSTOM.x)), 0, 14);
	float is_ring = INSTANCE_CUSTOM.z;
	float micro_layer = INSTANCE_CUSTOM.w;
	float spacing = u_base_spacing * exp2(float(level));
	vec2 cell = UV - vec2(u_grid_cells * 0.5);
	vec2 cell_offset_m = cell * spacing;
	vec2 level_center_m = round(u_lattice_center_plane / spacing) * spacing;
	vec2 anchor_offset_m = level_center_m + cell_offset_m;
	vec3 direction = direction_for_basis_offset(u_anchor_dir, u_anchor_right, u_anchor_up, anchor_offset_m);
	float biome = authored_biome_id(direction);
	float generated_height = 0.0;
	float shader_height = 0.0;
	#ifdef ASTERRA_AUTHORED_TERRAIN
	shader_height = u_shader_displacement_enabled > 0.5
		? author_displacement_height(direction, generated_height, biome, level, TIME)
		: 0.0;
	#endif
	shader_height += asterra_biome_profile_height(direction);
	float sink = is_ring > 0.5
		? clamp(spacing * 0.001 * max(u_sink_scale, 0.0), 0.004, 0.025)
		: 0.0;
	float cell_radius = length(cell);
	if (micro_layer < 0.5 && is_ring < 0.5 && level == 0 && cell_radius < MICRO_OUTER_L0_CELLS) {
		float overlap_sink = 1.0 - smootherstep_range(
			MICRO_HANDOFF_START_L0_CELLS, MICRO_OUTER_L0_CELLS, cell_radius);
		sink += overlap_sink * MICRO_L0_SINK_M;
	}
	float height = generated_height + shader_height - sink;
	float altitude_m = height + u_surface_bias;
	vec3 planet_position = direction * (u_planet_radius + altitude_m);
	vec3 render_position = u_anchor_render + stable_surface_offset(
		u_anchor_dir, u_anchor_right, u_anchor_up, anchor_offset_m, altitude_m);
	v_planet = planet_position;
	v_render = render_position;
	v_biome = biome;
	v_level = float(level);
	v_micro_layer = micro_layer;
	v_cap_valid = length(anchor_offset_m) / max(u_planet_radius, 1.0) <= u_visible_cap_angle * 1.03 ? 1.0 : 0.0;
	VERTEX = render_position;
}

void fragment() {
	if (v_cap_valid < 0.5) discard;
	vec3 up = normalize(v_planet);
	if (u_debug_side_cut > 0.5 && dot(up, u_anchor_up) > 0.0) discard;
	vec3 raw_normal = cross(dFdx(v_render), dFdy(v_render));
	vec3 world_normal = dot(raw_normal, raw_normal) > 1e-12 ? normalize(raw_normal) : up;
	if (dot(world_normal, up) < 0.0) world_normal = -world_normal;

	vec3 albedo = biome_color(v_biome);
	float roughness = 0.88;
	float specular = 0.22;
	float metallic = 0.0;
	float ao = 1.0;
	float slope = clamp(1.0 - dot(world_normal, up), 0.0, 1.0);
	float camera_distance = length(v_render - CAMERA_POSITION_WORLD);
	#ifdef ASTERRA_AUTHORED_TERRAIN
	// Blank terrain intentionally owns no generated context textures. Supply the
	// same neutral fallback context used by gpu_planet_context so every graph input
	// remains defined and deterministic while authored textures/material logic can
	// freely replace the blank surface.
	vec4 soil = vec4(0.45, 0.35, 0.20, 0.05);
	vec4 surface = vec4(0.08, 0.35, 0.25, 0.05);
	vec4 geology = vec4(0.45, 0.0, 0.0, 0.0);
	vec4 structure = vec4(0.5, 0.0, 0.0, 0.0);
	vec4 climate = vec4(0.55, 0.35, 0.45, 0.35);
	vec4 hydrology = vec4(0.5, 0.5, 0.0, 0.0);
	vec4 landform = vec4(0.0);
	vec4 primary = vec4(0.0);
	vec4 secondary = vec4(0.0);
	author_material_apply(up, v_planet,
		length(v_planet) - u_planet_radius,
		slope, v_biome, int(round(v_level)), camera_distance, TIME,
		soil, surface, geology, structure, climate, hydrology, landform,
		primary, secondary, 0.0, v_micro_layer, 0.0,
		albedo, world_normal, roughness, specular, metallic, ao);
	#endif

	NORMAL = normalize((VIEW_MATRIX * vec4(world_normal, 0.0)).xyz);
	ALBEDO = albedo;
	ROUGHNESS = roughness;
	SPECULAR = specular;
	METALLIC = metallic;
	AO = ao;
}
