shader_type spatial;
render_mode cull_disabled, diffuse_burley, specular_schlick_ggx;

// Textured ecological scatter for optimized runtime assets. Placement stays in
// the proven INSTANCE_ID vertex-GPU path until the indirect compact backend is
// repaired. This shader therefore deliberately targets sparse/medium-density
// assets and keeps the existing procedural grass/stone fallback intact.

#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/gpu_planet_context.gdshaderinc"
#include "res://shaders/gpu_geomorph.gdshaderinc"
#include "res://shaders/gpu_surface_classifier.gdshaderinc"
#include "res://shaders/gpu_scatter_common.gdshaderinc"

// Ecology roles. These are continuous environmental classifiers, not hard biome
// switches, so vegetation does not form nearest-filtered biome-cell borders.
// 0 grass, 1 fern/wet groundcover, 2 shrub, 3 arid plant,
// 4 geological rock, 5 deadwood, 6 tropical groundcover, 7 arid tree.
uniform int u_ecology_kind = 0;
uniform int u_asset_salt = 911;
uniform float u_asset_scale_min = 0.82;
uniform float u_asset_scale_max = 1.18;
uniform float u_asset_height_m = 1.0;
uniform float u_wind_strength = 0.0;
uniform float u_wind_speed = 1.25;

// Transform from the imported mesh node into the normalized asset root. Keeping
// it explicit avoids a CPU mesh rebake and preserves Blender/glTF child layouts.
uniform vec3 u_asset_axis_x = vec3(1.0, 0.0, 0.0);
uniform vec3 u_asset_axis_y = vec3(0.0, 1.0, 0.0);
uniform vec3 u_asset_axis_z = vec3(0.0, 0.0, 1.0);
uniform vec3 u_asset_origin = vec3(0.0);

uniform sampler2D u_source_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_source_normal : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_source_roughness : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_source_metallic : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D u_source_ao : filter_linear_mipmap_anisotropic, repeat_enable;
uniform int u_has_albedo = 0;
uniform int u_has_normal = 0;
uniform int u_has_roughness = 0;
uniform int u_has_metallic = 0;
uniform int u_has_ao = 0;
uniform int u_roughness_channel = 1;
uniform int u_metallic_channel = 2;
uniform int u_ao_channel = 0;
uniform vec4 u_albedo_color : source_color = vec4(1.0);
uniform float u_material_roughness = 0.85;
uniform float u_material_metallic = 0.0;
uniform float u_normal_scale = 1.0;
uniform float u_alpha_cutoff = 0.0;
uniform float u_specular = 0.18;

varying float v_alive;
varying float v_colour_variation;

float ecology_texture_channel(vec4 sample_value, int channel) {
	if (channel == 0) return sample_value.r;
	if (channel == 1) return sample_value.g;
	if (channel == 2) return sample_value.b;
	if (channel == 3) return sample_value.a;
	return dot(sample_value.rgb, vec3(0.3333333));
}

vec3 ecology_asset_point(vec3 p) {
	return u_asset_origin
		+ u_asset_axis_x * p.x
		+ u_asset_axis_y * p.y
		+ u_asset_axis_z * p.z;
}

vec3 ecology_asset_direction(vec3 v) {
	return normalize(
		u_asset_axis_x * v.x
		+ u_asset_axis_y * v.y
		+ u_asset_axis_z * v.z);
}

float ecology_suitability(float terrain_h, vec4 soil, vec4 surface,
		vec4 geology, vec4 structure, vec4 climate, vec4 hydro,
		vec4 landform, float biome) {
	float slope = sg_slope_proxy_deg(surface, structure, landform);
	float wet = surface.g;
	float biomass = surface.b;
	float soil_depth = asterra_ctx_soil_depth_m(surface);
	float temp = asterra_ctx_temperature_c(climate);
	float precip = asterra_ctx_precip_mm(climate);
	float arid = landform.g;
	float stable30 = 1.0 - smoothstep(24.0, 36.0, slope);
	float stable40 = 1.0 - smoothstep(31.0, 44.0, slope);
	float above_water = smoothstep(-0.35, 1.10, terrain_h);

	if (u_ecology_kind == 0) {
		return sg_grass_suitability(
			terrain_h, soil, surface, geology, structure, climate, hydro, landform, biome);
	}
	if (u_ecology_kind == 1) {
		float humid = smoothstep(0.30, 0.70, wet) * smoothstep(420.0, 1250.0, precip);
		float thermal = smoothstep(-4.0, 8.0, temp);
		return clamp(biomass * humid * thermal * stable30 * above_water, 0.0, 1.0);
	}
	if (u_ecology_kind == 2) {
		float rooted = smoothstep(0.10, 0.60, soil_depth);
		float climate_ok = mix(0.72, 1.0, smoothstep(250.0, 950.0, precip));
		climate_ok *= 1.0 - smoothstep(0.91, 1.0, arid);
		return clamp((0.28 + biomass * 0.88) * rooted * climate_ok
			* stable40 * above_water, 0.0, 1.0);
	}
	if (u_ecology_kind == 3) {
		float dry = smoothstep(0.42, 0.78, arid) * (1.0 - smoothstep(650.0, 1350.0, precip));
		float warm = smoothstep(0.0, 17.0, temp);
		return clamp((0.42 + biomass * 0.38) * dry * warm * stable30 * above_water, 0.0, 1.0);
	}
	if (u_ecology_kind == 4) {
		return sg_geologic_stone_suitability(
			terrain_h, soil, surface, geology, structure, climate, hydro, landform, biome);
	}
	if (u_ecology_kind == 5) {
		float forest_history = smoothstep(0.20, 0.64, biomass) * smoothstep(320.0, 1050.0, precip);
		float non_desert = 1.0 - smoothstep(0.84, 0.98, arid);
		return clamp(forest_history * non_desert
			* (1.0 - smoothstep(16.0, 30.0, slope)) * above_water, 0.0, 1.0);
	}
	if (u_ecology_kind == 6) {
		float tropical = smoothstep(14.0, 24.0, temp) * smoothstep(850.0, 1750.0, precip);
		float humid = smoothstep(0.45, 0.76, wet);
		return clamp(biomass * tropical * humid * stable30 * above_water, 0.0, 1.0);
	}
	// Sparse warm/arid tree analogue. Kept separate from low succulent/scrub so a
	// tree asset can use much lower density and wider spacing with the same shader.
	float dry_tree = smoothstep(0.40, 0.72, arid) * (1.0 - smoothstep(900.0, 1650.0, precip));
	float tree_temp = smoothstep(7.0, 21.0, temp);
	float rooted = smoothstep(0.18, 0.80, soil_depth);
	return clamp((0.30 + biomass * 0.55) * dry_tree * tree_temp * rooted
		* (1.0 - smoothstep(18.0, 31.0, slope)) * above_water, 0.0, 1.0);
}

void vertex() {
	vec3 source_vertex = ecology_asset_point(VERTEX);
	vec3 source_normal = ecology_asset_direction(NORMAL);
	vec3 source_tangent = ecology_asset_direction(TANGENT);
	vec3 source_binormal = ecology_asset_direction(BINORMAL);

	ivec2 absolute_cell;
	uint h;
	uint salt = uint(max(u_asset_salt, 1));
	vec3 d = sg_candidate_direction(INSTANCE_ID, salt, absolute_cell, h);
	vec2 terrain_sample = sg_terrain_sample(d);
	float macro_h = terrain_sample.x;
	float terrain_h = terrain_sample.y;
	vec4 soil = asterra_ctx_soil(d);
	vec4 surface = asterra_ctx_surface(d);
	vec4 geology = asterra_ctx_geology(d);
	vec4 structure = asterra_ctx_structure(d);
	vec4 climate = asterra_ctx_climate(d);
	vec4 hydro = asterra_ctx_hydrology(d);
	float biome = asterra_ctx_biome_id(d);
	vec4 landform = gm_landform_weights_from_context(
		macro_h, soil, surface, geology, structure, climate, hydro);
	float suitability = ecology_suitability(
		terrain_h, soil, surface, geology, structure, climate, hydro, landform, biome);
	v_alive = sg_acceptance(suitability, h, INSTANCE_ID);

	vec3 right;
	vec3 north;
	sg_tangent_basis(d, right, north);
	if (v_alive > 0.5) {
		float r0 = sg_hash_lane(h, 0u);
		float r1 = sg_hash_lane(h, 10u);
		float r2 = sg_hash_lane(h, 20u);
		float angle = r0 * TAU;
		float ca = cos(angle);
		float sa = sin(angle);
		float instance_scale = mix(u_asset_scale_min, u_asset_scale_max, r1);

		vec3 scaled = source_vertex * instance_scale;
		vec3 local_rotated = vec3(
			scaled.x * ca - scaled.z * sa,
			scaled.y,
			scaled.x * sa + scaled.z * ca);

		float tip = clamp(source_vertex.y / max(u_asset_height_m, 0.05), 0.0, 1.0);
		float phase = float(absolute_cell.x) * 0.233 + float(absolute_cell.y) * 0.371 + r2 * TAU;
		float wind = sin(TIME * u_wind_speed + phase) * u_wind_strength * tip * tip;
		local_rotated.x += wind;

		vec3 base = d * (u_scatter_planet_radius + terrain_h + u_scatter_surface_bias);
		VERTEX = base - u_scatter_origin
			+ right * local_rotated.x
			+ d * local_rotated.y
			+ north * local_rotated.z;

		vec3 n_rotated = vec3(
			source_normal.x * ca - source_normal.z * sa,
			source_normal.y,
			source_normal.x * sa + source_normal.z * ca);
		vec3 t_rotated = vec3(
			source_tangent.x * ca - source_tangent.z * sa,
			source_tangent.y,
			source_tangent.x * sa + source_tangent.z * ca);
		vec3 b_rotated = vec3(
			source_binormal.x * ca - source_binormal.z * sa,
			source_binormal.y,
			source_binormal.x * sa + source_binormal.z * ca);
		NORMAL = normalize(right * n_rotated.x + d * n_rotated.y + north * n_rotated.z);
		TANGENT = normalize(right * t_rotated.x + d * t_rotated.y + north * t_rotated.z);
		BINORMAL = normalize(right * b_rotated.x + d * b_rotated.y + north * b_rotated.z);
		v_colour_variation = mix(0.90, 1.08, sg_hash01(h ^ 0x72abu));
	} else {
		VERTEX = vec3(0.0);
		NORMAL = d;
		TANGENT = right;
		BINORMAL = north;
		v_colour_variation = 1.0;
	}
}

void fragment() {
	if (v_alive < 0.5) discard;
	vec4 texel = u_has_albedo != 0 ? texture(u_source_albedo, UV) : vec4(1.0);
	vec4 base = texel * u_albedo_color;
	if (base.a < u_alpha_cutoff) discard;

	ALBEDO = clamp(base.rgb * v_colour_variation, vec3(0.0), vec3(1.0));
	ROUGHNESS = u_material_roughness;
	if (u_has_roughness != 0) {
		ROUGHNESS *= ecology_texture_channel(texture(u_source_roughness, UV), u_roughness_channel);
	}
	ROUGHNESS = clamp(ROUGHNESS, 0.04, 1.0);

	METALLIC = u_material_metallic;
	if (u_has_metallic != 0) {
		METALLIC *= ecology_texture_channel(texture(u_source_metallic, UV), u_metallic_channel);
	}
	METALLIC = clamp(METALLIC, 0.0, 1.0);
	SPECULAR = u_specular;

	if (u_has_normal != 0) {
		NORMAL_MAP = texture(u_source_normal, UV).rgb;
		NORMAL_MAP_DEPTH = u_normal_scale;
	}
	if (u_has_ao != 0) {
		AO = ecology_texture_channel(texture(u_source_ao, UV), u_ao_channel);
		AO_LIGHT_AFFECT = 0.65;
	}
}
