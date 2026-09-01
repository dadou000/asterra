// Cached terrain-only material authoring post-pass.
// This file is intentionally not included by the shared PBR helper; it requires
// cached-clipmap varyings and the exact classification intermediates already
// computed by spherical_geometry_clipmap_cached_surface.gdshader.

#include "res://shaders/terrain_author_material_bytecode.gdshaderinc"

void author_cached_material_post(
		float biome,
		float camera_distance,
		vec3 geom_normal,
		vec4 soil,
		vec4 surface,
		vec4 geology,
		vec4 structure,
		vec4 climate,
		vec4 hydro,
		vec4 landform,
		vec4 primary,
		vec4 secondary,
		float rock_id,
		float rock_mix,
		float micro_layer,
		inout vec3 albedo,
		inout vec3 pbr_normal,
		inout float roughness,
		inout float specular,
		out float metallic,
		out float ao) {
	metallic = 0.0;
	ao = 1.0;
	vec3 planet_direction = normalize(v_planet);
	float slope = clamp(1.0 - dot(normalize(geom_normal), planet_direction), 0.0, 1.0);
	author_material_apply(planet_direction, v_planet,
		length(v_planet) - u_planet_radius, slope, biome,
		int(round(v_level)), camera_distance, TIME,
		soil, surface, geology, structure, climate, hydro, landform,
		primary, secondary, rock_id, micro_layer, rock_mix,
		albedo, pbr_normal, roughness, specular, metallic, ao);
}
