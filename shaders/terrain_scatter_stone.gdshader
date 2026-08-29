shader_type spatial;
render_mode diffuse_burley, specular_schlick_ggx;

#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/gpu_planet_context.gdshaderinc"
#include "res://shaders/gpu_geomorph.gdshaderinc"
#include "res://shaders/gpu_surface_classifier.gdshaderinc"
#include "res://shaders/gpu_scatter_common.gdshaderinc"

// 0 = geological / scree stone, 1 = river / depositional stone.
uniform int u_stone_kind = 0;
uniform float u_stone_scale = 1.0;

varying float v_alive;
varying vec3 v_stone_color;
varying float v_stone_roughness;

void vertex() {
	vec3 local_vertex = VERTEX;
	vec3 local_normal = NORMAL;
	uint salt = u_stone_kind == 0 ? 0x2b7u : 0x5d1u;
	ivec2 absolute_cell;
	uint h;
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
	float rock_id = asterra_ctx_rock_id(d);
	vec4 landform = gm_landform_weights_from_context(
		macro_h, soil, surface, geology, structure, climate, hydro);
	float suitability = u_stone_kind == 0
		? sg_geologic_stone_suitability(
			terrain_h, soil, surface, geology, structure, climate, hydro, landform, biome)
		: sg_river_stone_suitability(
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

		vec3 scale_xyz;
		if (u_stone_kind == 0) {
			float base_scale = mix(0.16, 0.92, r1 * r1) * u_stone_scale;
			float boulder_boost = smoothstep(0.83, 0.99, r2) * landform.r * 0.85;
			base_scale *= 1.0 + boulder_boost;
			scale_xyz = vec3(
				base_scale * mix(0.72, 1.22, r0),
				base_scale * mix(0.62, 1.18, r2),
				base_scale * mix(0.76, 1.28, r1));
		} else {
			float base_scale = mix(0.075, 0.34, r1 * r1) * u_stone_scale;
			scale_xyz = vec3(
				base_scale * mix(0.90, 1.35, r0),
				base_scale * mix(0.32, 0.62, r2),
				base_scale * mix(0.88, 1.32, r1));
		}

		vec3 scaled = local_vertex * scale_xyz;
		vec2 horizontal = vec2(scaled.x * ca - scaled.z * sa,
			scaled.x * sa + scaled.z * ca);
		vec3 base = d * (u_scatter_planet_radius + terrain_h + u_scatter_surface_bias);
		VERTEX = base - u_scatter_origin
			+ right * horizontal.x
			+ north * horizontal.y
			+ d * scaled.y;

		vec3 inv_scale_n = local_normal / max(scale_xyz, vec3(0.001));
		vec3 n_local = normalize(inv_scale_n);
		vec2 n_horizontal = vec2(n_local.x * ca - n_local.z * sa,
			n_local.x * sa + n_local.z * ca);
		NORMAL = normalize(right * n_horizontal.x + north * n_horizontal.y + d * n_local.y);

		vec3 rock_color = sc_rock_color(rock_id);
		float colour_jitter = mix(0.78, 1.17, sg_hash01(h ^ 0x6f21u));
		if (u_stone_kind == 0) {
			v_stone_color = clamp(rock_color * colour_jitter + vec3(0.012), vec3(0.015), vec3(0.62));
			v_stone_roughness = mix(0.72, 0.92, geology.r);
		} else {
			vec3 water_worn = mix(rock_color, vec3(0.255, 0.245, 0.225), 0.38);
			v_stone_color = clamp(water_worn * colour_jitter, vec3(0.02), vec3(0.60));
			v_stone_roughness = mix(0.60, 0.82, 1.0 - surface.g);
		}
	} else {
		VERTEX = vec3(0.0);
		NORMAL = d;
		v_stone_color = vec3(0.0);
		v_stone_roughness = 1.0;
	}
}

void fragment() {
	if (v_alive < 0.5) discard;
	ALBEDO = v_stone_color;
	ROUGHNESS = v_stone_roughness;
	SPECULAR = 0.18;
	METALLIC = 0.0;
}
