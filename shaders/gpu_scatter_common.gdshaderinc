// Shared deterministic GPU terrain-scatter candidate pipeline.
//
// The CPU only moves a snapped candidate window and binds immutable planet maps.
// Candidate position, jitter, terrain height, material suitability, scale and
// acceptance are derived on the GPU from absolute tangent-grid cells + world seed.
//
// Include order required by callers:
//   planet_surface_lookup.gdshaderinc
//   gpu_planet_context.gdshaderinc
//   gpu_geomorph.gdshaderinc
//   gpu_surface_classifier.gdshaderinc
//   gpu_scatter_common.gdshaderinc

uniform sampler2DArray u_scatter_macro_elevation : filter_linear_mipmap, repeat_disable;
uniform float u_scatter_macro_ready = 0.0;
uniform float u_scatter_macro_face_res = 0.0;

uniform float u_scatter_enabled = 1.0;
uniform float u_scatter_density = 1.0;
uniform float u_scatter_spacing = 1.25;
uniform int u_scatter_grid = 96;
uniform vec2 u_scatter_center_cell = vec2(0.0);
uniform vec3 u_scatter_anchor_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_scatter_anchor_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_scatter_anchor_up = vec3(0.0, 1.0, 0.0);
uniform float u_scatter_planet_radius = 1000000.0;
uniform vec3 u_scatter_origin = vec3(0.0);
uniform int u_scatter_seed = 1337;
uniform int u_scatter_detail_seed = 1337;
uniform float u_scatter_geomorph_spacing = 0.75;
uniform float u_scatter_surface_bias = 0.045;

float sg_hash01(uint h) {
	return float(h & 0x00ffffffu) / 16777215.0;
}

float sg_hash_lane(uint h, uint shift) {
	return float((h >> shift) & 0x000003ffu) / 1023.0;
}

ivec2 sg_local_cell(int instance_id) {
	int gx = instance_id % u_scatter_grid;
	int gy = instance_id / u_scatter_grid;
	int half_grid = u_scatter_grid / 2;
	return ivec2(gx - half_grid, gy - half_grid);
}

ivec2 sg_absolute_cell(int instance_id) {
	return ivec2(round(u_scatter_center_cell)) + sg_local_cell(instance_id);
}

uint sg_cell_hash(ivec2 absolute_cell, uint salt) {
	return gm_hash(ivec3(absolute_cell.x, absolute_cell.y, int(salt & 0x00007fffu)),
		uint(max(u_scatter_seed, 1)) + salt * 131u);
}

vec2 sg_cell_jitter(uint h) {
	return vec2(sg_hash_lane(h, 0u), sg_hash_lane(h, 10u)) - vec2(0.5);
}

float sg_window_fade(int instance_id) {
	vec2 q = vec2(sg_local_cell(instance_id)) / max(float(u_scatter_grid) * 0.5, 1.0);
	float r = length(q);
	return 1.0 - smoothstep(0.86, 1.0, r);
}

vec3 sg_direction_for_offset(vec2 offset_m) {
	return normalize(u_scatter_anchor_dir
		+ u_scatter_anchor_right * (offset_m.x / max(u_scatter_planet_radius, 1.0))
		+ u_scatter_anchor_up * (offset_m.y / max(u_scatter_planet_radius, 1.0)));
}

vec3 sg_candidate_direction(int instance_id, uint salt, out ivec2 absolute_cell,
		out uint cell_hash) {
	absolute_cell = sg_absolute_cell(instance_id);
	cell_hash = sg_cell_hash(absolute_cell, salt);
	vec2 jitter = sg_cell_jitter(cell_hash) * (u_scatter_spacing * 0.72);
	vec2 offset_m = vec2(absolute_cell) * u_scatter_spacing + jitter;
	return sg_direction_for_offset(offset_m);
}

vec3 sg_macro_coord(vec3 dir) {
	vec3 fuv = surface_face_uv(dir);
	ivec3 size3 = textureSize(u_scatter_macro_elevation, 0);
	float tex_res = float(size3.x);
	float gutter = max((tex_res - u_scatter_macro_face_res) * 0.5, 0.0);
	vec2 uv = (fuv.xy * u_scatter_macro_face_res + vec2(gutter)) / max(tex_res, 1.0);
	return vec3(uv, fuv.z);
}

float sg_macro_height(vec3 dir) {
	if (u_scatter_macro_ready < 0.5 || u_scatter_macro_face_res <= 0.5) return 0.0;
	return textureLod(u_scatter_macro_elevation, sg_macro_coord(dir), 0.0).r;
}

float sg_terrain_height(vec3 dir, float macro_h) {
	return macro_h + gm_geomorph_height(
		dir, u_scatter_planet_radius, u_scatter_geomorph_spacing, macro_h,
		uint(max(u_scatter_detail_seed, 1)));
}

void sg_tangent_basis(vec3 dir, out vec3 right, out vec3 north) {
	vec3 basis_up = abs(dir.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	right = normalize(cross(basis_up, dir));
	north = normalize(cross(dir, right));
}

float sg_slope_proxy_deg(vec4 surface, vec4 structure, vec4 landform) {
	float soil_depth = asterra_ctx_soil_depth_m(surface);
	float thin = 1.0 - smoothstep(0.08, 0.70, soil_depth);
	float exposed = thin * (1.0 - surface.b * 0.55);
	return clamp(
		landform.r * 23.0
		+ structure.g * 13.0
		+ exposed * 9.0,
		0.0, 52.0);
}

float sg_grass_suitability(float terrain_h, vec4 soil, vec4 surface, vec4 geology,
		vec4 structure, vec4 climate, vec4 hydro, vec4 landform, float biome) {
	float slope_proxy = sg_slope_proxy_deg(surface, structure, landform);
	vec4 primary = sc_primary_weights(
		soil, surface, geology, structure, climate, hydro, landform, biome, slope_proxy);
	vec4 secondary = sc_secondary_weights(
		soil, surface, geology, structure, climate, hydro, landform, biome, slope_proxy);
	float land = smoothstep(-0.35, 1.5, terrain_h);
	float wetland_penalty = mix(1.0, 0.48, structure.a);
	float mud_penalty = 1.0 - secondary.r * 0.72;
	float snow_penalty = 1.0 - secondary.g;
	float bare_penalty = 1.0 - sc_is_biome(biome, 18.0);
	float ice_penalty = 1.0 - sc_is_biome(biome, 3.0);
	return clamp(primary.b * wetland_penalty * mud_penalty * snow_penalty
		* bare_penalty * ice_penalty * land, 0.0, 1.0);
}

float sg_geologic_stone_suitability(float terrain_h, vec4 soil, vec4 surface,
		vec4 geology, vec4 structure, vec4 climate, vec4 hydro,
		vec4 landform, float biome) {
	float slope_proxy = sg_slope_proxy_deg(surface, structure, landform);
	vec4 primary = sc_primary_weights(
		soil, surface, geology, structure, climate, hydro, landform, biome, slope_proxy);
	vec4 secondary = sc_secondary_weights(
		soil, surface, geology, structure, climate, hydro, landform, biome, slope_proxy);
	float soil_depth = asterra_ctx_soil_depth_m(surface);
	float thin = 1.0 - smoothstep(0.10, 0.85, soil_depth);
	float exposure = max(primary.r * 0.72, secondary.b * 1.10);
	exposure = max(exposure, landform.r * thin * (0.38 + geology.r * 0.52));
	exposure *= 1.0 - secondary.g * 0.88;
	exposure *= smoothstep(-0.25, 1.0, terrain_h);
	return clamp(exposure, 0.0, 1.0);
}

float sg_river_stone_suitability(float terrain_h, vec4 soil, vec4 surface,
		vec4 geology, vec4 structure, vec4 climate, vec4 hydro,
		vec4 landform, float biome) {
	float slope_proxy = min(sg_slope_proxy_deg(surface, structure, landform), 28.0);
	vec4 secondary = sc_secondary_weights(
		soil, surface, geology, structure, climate, hydro, landform, biome, slope_proxy);
	float channel = hydro.b * (0.35 + hydro.a * 0.95);
	float gravel = max(secondary.a, channel * (0.35 + surface.a * 0.65));
	float mud_penalty = 1.0 - secondary.r * 0.70;
	float land = smoothstep(-0.50, 0.70, terrain_h);
	return clamp(gravel * mud_penalty * land, 0.0, 1.0);
}

float sg_acceptance(float suitability, uint h, int instance_id) {
	float threshold = clamp(suitability * u_scatter_density * sg_window_fade(instance_id), 0.0, 0.985);
	float r = sg_hash01(gm_hash(ivec3(int(h & 0x0000ffffu), instance_id, u_scatter_seed), h ^ 0x9e37u));
	return r < threshold && u_scatter_enabled > 0.5 ? 1.0 : 0.0;
}
