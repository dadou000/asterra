// Cached terrain-only material authoring post-pass.
// This file is intentionally not included by the shared PBR helper; it requires
// the cached clipmap's v_planet/v_level varyings and therefore belongs to that
// shader path only.

#include "res://shaders/terrain_author_material_bytecode.gdshaderinc"

void author_cached_material_post(
		float biome,
		float camera_distance,
		vec3 geom_normal,
		inout vec3 albedo,
		inout vec3 pbr_normal,
		inout float roughness) {
	float authored_metallic = 0.0;
	float authored_ao = 1.0;
	vec3 planet_direction = normalize(v_planet);
	float slope = clamp(1.0 - dot(normalize(geom_normal), planet_direction), 0.0, 1.0);
	author_material_apply(planet_direction, v_planet,
		length(v_planet) - u_planet_radius, slope, biome,
		int(round(v_level)), camera_distance, TIME,
		albedo, pbr_normal, roughness, authored_metallic, authored_ao);
}
