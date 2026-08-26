// Immutable six-face coarse planet context.
// Built once from PlanetFields and then sampled directly by runtime terrain shaders.

uniform sampler2DArray u_ctx_soil : filter_linear, repeat_disable;
uniform sampler2DArray u_ctx_surface : filter_linear, repeat_disable;
uniform sampler2DArray u_ctx_geology : filter_linear, repeat_disable;
uniform sampler2DArray u_ctx_structure : filter_linear, repeat_disable;
uniform sampler2DArray u_ctx_climate : filter_linear, repeat_disable;
uniform sampler2DArray u_ctx_hydrology : filter_linear, repeat_disable;
uniform sampler2DArray u_ctx_rock : filter_nearest, repeat_disable;
uniform sampler2DArray u_ctx_biome : filter_nearest, repeat_disable;
uniform float u_ctx_ready = 0.0;
uniform float u_ctx_face_res = 0.0;

vec3 asterra_context_coord(vec3 dir) {
	vec3 fuv = surface_face_uv(dir);
	ivec3 size3 = textureSize(u_ctx_soil, 0);
	float tex_res = float(size3.x);
	float gutter = max((tex_res - u_ctx_face_res) * 0.5, 0.0);
	vec2 uv = (fuv.xy * u_ctx_face_res + vec2(gutter)) / max(tex_res, 1.0);
	return vec3(uv, fuv.z);
}

vec4 asterra_ctx_soil(vec3 dir) {
	if (u_ctx_ready < 0.5) return vec4(0.45, 0.35, 0.20, 0.05);
	return textureLod(u_ctx_soil, asterra_context_coord(dir), 0.0);
}

vec4 asterra_ctx_surface(vec3 dir) {
	if (u_ctx_ready < 0.5) return vec4(0.08, 0.35, 0.25, 0.05);
	return textureLod(u_ctx_surface, asterra_context_coord(dir), 0.0);
}

vec4 asterra_ctx_geology(vec3 dir) {
	if (u_ctx_ready < 0.5) return vec4(0.45, 0.0, 0.0, 0.0);
	return textureLod(u_ctx_geology, asterra_context_coord(dir), 0.0);
}

vec4 asterra_ctx_structure(vec3 dir) {
	if (u_ctx_ready < 0.5) return vec4(0.5, 0.0, 0.0, 0.0);
	return textureLod(u_ctx_structure, asterra_context_coord(dir), 0.0);
}

vec4 asterra_ctx_climate(vec3 dir) {
	if (u_ctx_ready < 0.5) return vec4(0.55, 0.35, 0.45, 0.35);
	return textureLod(u_ctx_climate, asterra_context_coord(dir), 0.0);
}

vec4 asterra_ctx_hydrology(vec3 dir) {
	if (u_ctx_ready < 0.5) return vec4(0.5, 0.5, 0.0, 0.0);
	return textureLod(u_ctx_hydrology, asterra_context_coord(dir), 0.0);
}

float asterra_ctx_rock_id(vec3 dir) {
	if (u_ctx_ready < 0.5) return 0.0;
	return floor(textureLod(u_ctx_rock, asterra_context_coord(dir), 0.0).r * 12.0 + 0.5);
}

float asterra_ctx_biome_id(vec3 dir) {
	if (u_ctx_ready < 0.5) return 7.0;
	return floor(textureLod(u_ctx_biome, asterra_context_coord(dir), 0.0).r * 19.0 + 0.5);
}

float asterra_ctx_soil_depth_m(vec4 surface) {
	return surface.r * 14.0;
}

float asterra_ctx_sediment_m(vec4 surface) {
	return surface.a * 160.0;
}

float asterra_ctx_temperature_c(vec4 climate) {
	return mix(-40.0, 50.0, climate.r);
}

float asterra_ctx_precip_mm(vec4 climate) {
	return climate.g * 3000.0;
}

float asterra_ctx_temp_range_c(vec4 climate) {
	return climate.a * 55.0;
}

float asterra_ctx_erodibility(vec4 geology) {
	return geology.r * 1.85;
}

float asterra_ctx_strata_dip(vec4 geology) {
	return geology.g * 1.2;
}

float asterra_ctx_uplift_signed(vec4 structure) {
	return structure.r * 2.0 - 1.0;
}

vec2 asterra_ctx_flow(vec4 hydro) {
	return hydro.rg * 2.0 - 1.0;
}
