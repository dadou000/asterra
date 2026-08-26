// Shared deterministic GPU terrain-scatter candidate pipeline.
// CPU moves only a snapped candidate window; placement/classification stays GPU-side.

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

float sg_hash01(uint h) { return float(h & 0x00ffffffu) / 16777215.0; }
float sg_hash_lane(uint h, uint shift) { return float((h >> shift) & 0x000003ffu) / 1023.0; }

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
	return 1.0 - smoothstep(0.86, 1.0, length(q));
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

vec4 sg_cubic_weights(float t) {
	float t2 = t * t;
	float t3 = t2 * t;
	return vec4((1.0 - 3.0*t + 3.0*t2 - t3) / 6.0,
		(4.0 - 6.0*t2 + 3.0*t3) / 6.0,
		(1.0 + 3.0*t + 3.0*t2 - 3.0*t3) / 6.0,
		t3 / 6.0);
}

// Same four-fetch cubic B-spline reconstruction used by the active terrain at L0.
float sg_macro_bicubic(vec3 coord) {
	ivec3 size3 = textureSize(u_scatter_macro_elevation, 0);
	vec2 size2 = vec2(size3.xy);
	vec2 p = coord.xy * size2 - vec2(0.5);
	vec2 base = floor(p);
	vec2 f = fract(p);
	vec4 wx = sg_cubic_weights(f.x);
	vec4 wy = sg_cubic_weights(f.y);
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
	float s00 = textureLod(u_scatter_macro_elevation, vec3(uv00, coord.z), 0.0).r;
	float s10 = textureLod(u_scatter_macro_elevation, vec3(uv10, coord.z), 0.0).r;
	float s01 = textureLod(u_scatter_macro_elevation, vec3(uv01, coord.z), 0.0).r;
	float s11 = textureLod(u_scatter_macro_elevation, vec3(uv11, coord.z), 0.0).r;
	return mix(mix(s00, s10, gx1), mix(s01, s11, gx1), gy1);
}

float sg_macro_height(vec3 dir) {
	if (u_scatter_macro_ready < 0.5 || u_scatter_macro_face_res <= 0.5) return 0.0;
	return sg_macro_bicubic(sg_macro_coord(dir));
}

float sg_terrain_height(vec3 dir, float macro_h) {
	return macro_h + gm_geomorph_height(dir, u_scatter_planet_radius,
		u_scatter_geomorph_spacing, macro_h, uint(max(u_scatter_detail_seed, 1)));
}

void sg_tangent_basis(vec3 dir, out vec3 right, out vec3 north) {
	vec3 basis_up = abs(dir.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	right = normalize(cross(basis_up, dir));
	north = normalize(cross(dir, right));
}

float sg_slope_proxy_deg(vec4 surface, vec4 structure, vec4 landform) {
	float thin = 1.0 - smoothstep(0.08, 0.70, asterra_ctx_soil_depth_m(surface));
	float exposed = thin * (1.0 - surface.b * 0.55);
	return clamp(landform.r * 23.0 + structure.g * 13.0 + exposed * 9.0, 0.0, 52.0);
}

float sg_grass_suitability(float terrain_h, vec4 soil, vec4 surface, vec4 geology,
		vec4 structure, vec4 climate, vec4 hydro, vec4 landform, float biome) {
	float slope_proxy = sg_slope_proxy_deg(surface, structure, landform);
	vec4 primary = sc_primary_weights(soil, surface, geology, structure, climate, hydro, landform, biome, slope_proxy);
	vec4 secondary = sc_secondary_weights(soil, surface, geology, structure, climate, hydro, landform, biome, slope_proxy);
	float land = smoothstep(-0.35, 1.5, terrain_h);
	return clamp(primary.b * mix(1.0, 0.48, structure.a)
		* (1.0 - secondary.r * 0.72) * (1.0 - secondary.g) * land, 0.0, 1.0);
}

float sg_geologic_stone_suitability(float terrain_h, vec4 soil, vec4 surface,
		vec4 geology, vec4 structure, vec4 climate, vec4 hydro,
		vec4 landform, float biome) {
	float slope_proxy = sg_slope_proxy_deg(surface, structure, landform);
	vec4 primary = sc_primary_weights(soil, surface, geology, structure, climate, hydro, landform, biome, slope_proxy);
	vec4 secondary = sc_secondary_weights(soil, surface, geology, structure, climate, hydro, landform, biome, slope_proxy);
	float thin = 1.0 - smoothstep(0.10, 0.85, asterra_ctx_soil_depth_m(surface));
	float exposure = max(primary.r * 0.72, secondary.b * 1.10);
	exposure = max(exposure, landform.r * thin * (0.38 + geology.r * 0.52));
	exposure *= 1.0 - secondary.g * 0.88;
	return clamp(exposure * smoothstep(-0.25, 1.0, terrain_h), 0.0, 1.0);
}

float sg_river_stone_suitability(float terrain_h, vec4 soil, vec4 surface,
		vec4 geology, vec4 structure, vec4 climate, vec4 hydro,
		vec4 landform, float biome) {
	float slope_proxy = min(sg_slope_proxy_deg(surface, structure, landform), 28.0);
	vec4 secondary = sc_secondary_weights(soil, surface, geology, structure, climate, hydro, landform, biome, slope_proxy);
	float channel = hydro.b * (0.35 + hydro.a * 0.95);
	float gravel = max(secondary.a, channel * (0.35 + surface.a * 0.65));
	return clamp(gravel * (1.0 - secondary.r * 0.70) * smoothstep(-0.50, 0.70, terrain_h), 0.0, 1.0);
}

float sg_acceptance(float suitability, uint h, int instance_id) {
	float threshold = clamp(suitability * u_scatter_density * sg_window_fade(instance_id), 0.0, 0.985);
	// World-cell random decision only: MultiMesh slot identity is intentionally absent.
	float r = sg_hash01(gm_hash(ivec3(int(h & 0xffffu), int((h >> 16u) & 0xffffu), u_scatter_seed), h ^ 0x9e37u));
	return r < threshold && u_scatter_enabled > 0.5 ? 1.0 : 0.0;
}