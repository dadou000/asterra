#[compute]
#version 450

// Exact near/mid-field contact query against the same height stack used by the
// production clipmap vertex shader. The procedural base is read from the active
// toroidal terrain cache with the identical validity key and bilinear lattice
// sampling. Persistent and active deformation textures are then sampled with the
// same gnomonic projection as terrain_edit_delta.gdshaderinc.
//
// Result: x = rendered altitude MSL (including surface bias),
//         y = 1 when every required cache texel was valid, otherwise 0,
//         z = selected logical LOD, w = morph weight.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2DArray terrain_cache;
layout(set = 0, binding = 1) uniform sampler2D persistent_edit;
layout(set = 0, binding = 2) uniform sampler2D active_deform;

layout(set = 0, binding = 3, std430) readonly buffer QueryBuffer {
	vec4 data[];
} queries;

layout(set = 0, binding = 4, std430) writeonly buffer ResultBuffer {
	vec4 data[];
} results;

layout(set = 0, binding = 5, std140) uniform Params {
	vec4 p[12];
} params;

const int ASTERRA_MAX_LEVEL = 14;
const float FINE_MORPH_START_Q = 0.875;

float smootherstep01(float x) {
	x = clamp(x, 0.0, 1.0);
	return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

float smootherstep_range(float edge0, float edge1, float x) {
	return smootherstep01((x - edge0) / max(edge1 - edge0, 1e-9));
}

uint terrain_cache_key(ivec2 cell, int level, int generation) {
	uint h = uint(cell.x) * 0x8da6b343u;
	h ^= uint(cell.y) * 0xd8163841u;
	h ^= uint(level + 1) * 0xcb1ab31fu;
	h ^= uint(generation) * 0x9e3779b9u;
	h ^= h >> uint(16);
	h *= 0x7feb352du;
	h ^= h >> uint(15);
	h *= 0x846ca68bu;
	h ^= h >> uint(16);
	return (h & 0x00ffffffu) + 1u;
}

ivec2 terrain_cache_coord(ivec2 cell, int cache_res) {
	float r = float(max(cache_res, 1));
	return ivec2(mod(vec2(cell), vec2(r)));
}

bool cache_fetch_exact(ivec2 cell, int level, int generation, int cache_res,
		out vec2 sample) {
	sample = vec2(0.0);
	if (level < 0 || level > ASTERRA_MAX_LEVEL) return false;
	vec4 cached = texelFetch(terrain_cache,
		ivec3(terrain_cache_coord(cell, cache_res), level), 0);
	float expected = float(terrain_cache_key(cell, level, generation));
	if (abs(cached.b - expected) > 0.25) return false;
	// Renderer cache storage: R=final procedural height, G=macro height.
	sample = vec2(cached.g, cached.r);
	return true;
}

bool cache_fetch_bilinear(vec2 lattice_cell, int level, int generation,
		int cache_res, out vec2 sample) {
	ivec2 c00 = ivec2(floor(lattice_cell));
	vec2 f = fract(lattice_cell);
	vec2 s00;
	if (max(f.x, f.y) <= 1e-5) {
		return cache_fetch_exact(c00, level, generation, cache_res, sample);
	}
	vec2 s10;
	vec2 s01;
	vec2 s11;
	if (!cache_fetch_exact(c00, level, generation, cache_res, s00)
			|| !cache_fetch_exact(c00 + ivec2(1, 0), level, generation, cache_res, s10)
			|| !cache_fetch_exact(c00 + ivec2(0, 1), level, generation, cache_res, s01)
			|| !cache_fetch_exact(c00 + ivec2(1, 1), level, generation, cache_res, s11)) {
		sample = vec2(0.0);
		return false;
	}
	sample = mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);
	return true;
}

float projected_edit_delta(vec3 d, vec3 center, vec3 right, vec3 up,
		float half_extent_m, float planet_radius, sampler2D field_tex) {
	if (half_extent_m <= 0.5) return 0.0;
	vec3 c = normalize(center);
	float denom = dot(d, c);
	if (denom <= 0.01) return 0.0;
	vec2 plane_m = vec2(dot(d, normalize(right)), dot(d, normalize(up)))
		/ denom * planet_radius;
	vec2 uv = plane_m / (half_extent_m * 2.0) + vec2(0.5);
	if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) {
		return 0.0;
	}
	return textureLod(field_tex, uv, 0.0).r;
}

void main() {
	uint query_index = gl_GlobalInvocationID.x;
	int query_count = int(params.p[0].x + 0.5);
	if (query_index >= uint(max(query_count, 0))) return;

	float planet_radius = params.p[0].y;
	float base_spacing = max(params.p[0].z, 1e-6);
	float grid_cells = max(params.p[0].w, 2.0);
	vec3 anchor_dir = normalize(params.p[1].xyz);
	int cache_generation = int(params.p[1].w + 0.5);
	vec3 anchor_right = normalize(params.p[2].xyz);
	bool cache_ready = params.p[2].w > 0.5;
	vec3 anchor_up = normalize(params.p[3].xyz);
	float surface_bias = params.p[3].w;
	vec2 lattice_center_plane = params.p[4].xy;
	int active_min = clamp(int(params.p[4].z + 0.5), 0, ASTERRA_MAX_LEVEL);
	int active_max = clamp(int(params.p[4].w + 0.5), active_min, ASTERRA_MAX_LEVEL);

	vec3 d = normalize(queries.data[query_index].xyz);
	if (!cache_ready || planet_radius <= 1.0) {
		results.data[query_index] = vec4(0.0, 0.0, -1.0, 0.0);
		return;
	}
	float denom = dot(d, anchor_dir);
	if (denom <= 0.01) {
		results.data[query_index] = vec4(0.0, 0.0, -1.0, 0.0);
		return;
	}
	vec2 anchor_offset_m = vec2(dot(d, anchor_right), dot(d, anchor_up))
		/ denom * planet_radius;

	float half_cells = grid_cells * 0.5;
	int selected_level = -1;
	vec2 selected_cell = vec2(0.0);
	float selected_spacing = base_spacing;
	for (int level = 0; level <= ASTERRA_MAX_LEVEL; ++level) {
		if (level < active_min || level > active_max) continue;
		float spacing = base_spacing * exp2(float(level));
		vec2 level_center_m = round(lattice_center_plane / spacing) * spacing;
		vec2 cell = (anchor_offset_m - level_center_m) / spacing;
		if (length(cell) <= half_cells + 1e-4) {
			selected_level = level;
			selected_cell = cell;
			selected_spacing = spacing;
			break;
		}
	}
	if (selected_level < 0) {
		results.data[query_index] = vec4(0.0, 0.0, -1.0, 0.0);
		return;
	}

	vec2 fine;
	vec2 lattice_cell = anchor_offset_m / selected_spacing;
	int cache_res = max(int(params.p[11].x + 0.5), 1);
	if (!cache_fetch_bilinear(lattice_cell, selected_level, cache_generation,
			cache_res, fine)) {
		results.data[query_index] = vec4(0.0, 0.0, float(selected_level), 0.0);
		return;
	}

	float morph = smootherstep_range(FINE_MORPH_START_Q, 1.0,
		length(selected_cell) / max(half_cells, 1.0));
	if (selected_level < ASTERRA_MAX_LEVEL && morph > 0.001) {
		float coarse_spacing = base_spacing * exp2(float(selected_level + 1));
		vec2 coarse;
		if (!cache_fetch_bilinear(anchor_offset_m / coarse_spacing,
				selected_level + 1, cache_generation, cache_res, coarse)) {
			results.data[query_index] = vec4(0.0, 0.0, float(selected_level), morph);
			return;
		}
		fine = mix(fine, coarse, morph);
	}

	float h = fine.y;
	if (params.p[5].w > 0.5) {
		h += projected_edit_delta(d, params.p[5].xyz, params.p[6].xyz,
			params.p[7].xyz, params.p[6].w, planet_radius, persistent_edit);
	}
	if (params.p[8].w > 0.5) {
		h += projected_edit_delta(d, params.p[8].xyz, params.p[9].xyz,
			params.p[10].xyz, params.p[9].w, planet_radius, active_deform);
	}

	// The topmost near-field surface has no ring handoff sink. Inside the L0
	// micro-disc the micro layer is the visible surface and likewise has no overlap
	// sink. Therefore the final visible altitude is h + the renderer surface bias.
	results.data[query_index] = vec4(h + surface_bias, 1.0,
		float(selected_level), morph);
}
