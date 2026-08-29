shader_type spatial;
render_mode cull_disabled, diffuse_burley, specular_schlick_ggx;

#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/gpu_planet_context.gdshaderinc"
#include "res://shaders/gpu_geomorph.gdshaderinc"
#include "res://shaders/gpu_surface_classifier.gdshaderinc"
#include "res://shaders/gpu_scatter_common.gdshaderinc"

uniform float u_grass_height_min = 0.22;
uniform float u_grass_height_max = 0.82;
uniform float u_grass_width = 0.055;
uniform float u_grass_wind_strength = 0.055;
uniform float u_grass_wind_speed = 1.7;

varying float v_alive;
varying vec3 v_grass_color;

void vertex() {
	vec3 local_vertex = VERTEX;
	vec3 local_normal = NORMAL;
	ivec2 absolute_cell;
	uint h;
	vec3 d = sg_candidate_direction(INSTANCE_ID, 0x117u, absolute_cell, h);
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
	float suitability = sg_grass_suitability(
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
		vec2 horizontal = vec2(local_vertex.x * ca - local_vertex.z * sa,
			local_vertex.x * sa + local_vertex.z * ca);
		float biome_short = max(sc_is_biome(biome, 17.0), sc_is_biome(biome, 11.0));
		float height_m = mix(u_grass_height_min, u_grass_height_max, 0.25 + r1 * 0.75);
		height_m *= mix(1.0, 0.62, biome_short);
		float width_m = u_grass_width * mix(0.72, 1.28, r2);

		float tip = clamp(local_vertex.y, 0.0, 1.0);
		float phase = float(absolute_cell.x) * 0.37 + float(absolute_cell.y) * 0.21 + r2 * TAU;
		float wind = sin(TIME * u_grass_wind_speed + phase) * u_grass_wind_strength * tip * tip;
		horizontal.x += wind;
		vec3 base = d * (u_scatter_planet_radius + terrain_h + u_scatter_surface_bias);
		VERTEX = base - u_scatter_origin
			+ right * (horizontal.x * width_m)
			+ north * (horizontal.y * width_m)
			+ d * (local_vertex.y * height_m);

		vec3 n_local = vec3(
			local_normal.x * ca - local_normal.z * sa,
			local_normal.y,
			local_normal.x * sa + local_normal.z * ca);
		NORMAL = normalize(right * n_local.x + d * n_local.y + north * n_local.z);
		vec3 base_color = sc_vegetation_color(surface, climate, biome);
		float variation = mix(0.82, 1.15, sg_hash01(h ^ 0x4ad3u));
		v_grass_color = clamp(base_color * variation, vec3(0.018), vec3(0.42));
	} else {
		VERTEX = vec3(0.0);
		NORMAL = d;
		v_grass_color = vec3(0.0);
	}
}

void fragment() {
	if (v_alive < 0.5) discard;
	ALBEDO = v_grass_color;
	ROUGHNESS = 0.96;
	SPECULAR = 0.08;
	METALLIC = 0.0;
}
