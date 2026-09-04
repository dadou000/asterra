// Immutable six-face coarse planet context.
// Built once from PlanetFields and then sampled directly by runtime terrain shaders.
// Planet Studio can additionally bind a high-resolution camera-centred categorical
// biome override. The generated context texture remains pristine; authoring data is
// a disposable overlay rebuilt from sparse spherical brush strokes.

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

uniform sampler2D u_author_biome_override : filter_nearest, repeat_disable;
uniform float u_author_biome_ready = 0.0;
uniform vec3 u_author_biome_center = vec3(1.0, 0.0, 0.0);
uniform vec3 u_author_biome_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_author_biome_up = vec3(0.0, 1.0, 0.0);
uniform float u_author_biome_half_extent_m = 512.0;
uniform float u_author_biome_planet_radius = 1000000.0;

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

float asterra_generated_biome_id(vec3 dir) {
	if (u_ctx_ready < 0.5) return 7.0;
	return floor(textureLod(u_ctx_biome, asterra_context_coord(dir), 0.0).r * 19.0 + 0.5);
}

float asterra_author_biome_id(vec3 dir, float generated_biome) {
	if (u_author_biome_ready < 0.5) return generated_biome;
	vec3 d = normalize(dir);
	float denom = dot(d, normalize(u_author_biome_center));
	if (denom <= 0.01) return generated_biome;
	float radius = max(u_author_biome_planet_radius, 1.0);
	vec2 plane_m = vec2(
		dot(d, normalize(u_author_biome_right)),
		dot(d, normalize(u_author_biome_up))) * (radius / denom);
	float half_extent = max(u_author_biome_half_extent_m, 0.001);
	if (abs(plane_m.x) > half_extent || abs(plane_m.y) > half_extent)
		return generated_biome;
	// Image rows increase downward; the tangent up vector increases upward.
	vec2 uv = vec2(plane_m.x, -plane_m.y) / (half_extent * 2.0) + vec2(0.5);
	vec2 authored = textureLod(u_author_biome_override, uv, 0.0).rg;
	if (authored.g < 0.5) return generated_biome;
	return floor(authored.r * 19.0 + 0.5);
}

float asterra_ctx_biome_id(vec3 dir) {
	return asterra_author_biome_id(dir, asterra_generated_biome_id(dir));
}

// Bilinearly-filtered 0..1 membership of `biome` in the generated biome field.
// The id texture is filter_nearest, so this reads the four surrounding texels,
// maps each to a hard 0/1, and blends with the sub-texel fraction -- a genuinely
// C0-continuous ramp about one context texel wide, with no terracing. Inside an
// active authored biome override the edge is deliberately hard, so fall back to
// the categorical id there.
float asterra_ctx_biome_membership(vec3 dir, int biome) {
	float generated_here = asterra_generated_biome_id(dir);
	if (u_author_biome_ready > 0.5
			&& asterra_author_biome_id(dir, generated_here) != generated_here) {
		return (int(asterra_author_biome_id(dir, generated_here)) == biome) ? 1.0 : 0.0;
	}
	if (u_ctx_ready < 0.5) {
		return (7 == biome) ? 1.0 : 0.0;
	}
	vec3 coord = asterra_context_coord(dir);
	ivec3 size3 = textureSize(u_ctx_biome, 0);
	int layer = int(coord.z + 0.5);
	vec2 p = coord.xy * vec2(size3.xy) - vec2(0.5);
	vec2 f = fract(p);
	ivec2 b = ivec2(floor(p));
	ivec2 hi = size3.xy - ivec2(1);
	float m00 = (int(floor(texelFetch(u_ctx_biome, ivec3(clamp(b, ivec2(0), hi), layer), 0).r * 19.0 + 0.5)) == biome) ? 1.0 : 0.0;
	float m10 = (int(floor(texelFetch(u_ctx_biome, ivec3(clamp(b + ivec2(1, 0), ivec2(0), hi), layer), 0).r * 19.0 + 0.5)) == biome) ? 1.0 : 0.0;
	float m01 = (int(floor(texelFetch(u_ctx_biome, ivec3(clamp(b + ivec2(0, 1), ivec2(0), hi), layer), 0).r * 19.0 + 0.5)) == biome) ? 1.0 : 0.0;
	float m11 = (int(floor(texelFetch(u_ctx_biome, ivec3(clamp(b + ivec2(1, 1), ivec2(0), hi), layer), 0).r * 19.0 + 0.5)) == biome) ? 1.0 : 0.0;
	return mix(mix(m00, m10, f.x), mix(m01, m11, f.x), f.y);
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
