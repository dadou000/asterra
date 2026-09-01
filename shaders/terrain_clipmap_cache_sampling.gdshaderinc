// Toroidal GPU terrain-cache sampling for the spherical geometry clipmap.
// Include this after the analytic height helpers; cache misses deliberately fall
// back to those helpers so staggered warm-up can never expose stale geometry.
//
// IMPORTANT: the visible clipmap is a triangle mesh, not a bilinear height patch.
// Fine vertices that morph to their parent therefore have to converge to the
// parent's actual piecewise-planar triangles. Bilinear interpolation of the four
// parent samples is a different surface between lattice vertices and creates the
// classic alternating zipper crack at every LOD boundary.

#include "res://shaders/terrain_author_procedural_displacement.gdshaderinc"

uniform sampler2DArray u_terrain_cache : filter_nearest, repeat_disable;
uniform float u_terrain_cache_ready = 0.0;
uniform int u_terrain_cache_generation = 1;
uniform int u_terrain_cache_res = 512;

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

ivec2 terrain_cache_coord(ivec2 cell) {
	float r = float(max(u_terrain_cache_res, 1));
	return ivec2(mod(vec2(cell), vec2(r)));
}

bool terrain_cache_fetch_exact(ivec2 cell, int level, out vec2 sample) {
	sample = vec2(0.0);
	if (u_terrain_cache_ready < 0.5 || level < 0 || level > ASTERRA_MAX_LEVEL) {
		return false;
	}
	vec4 cached = texelFetch(u_terrain_cache,
		ivec3(terrain_cache_coord(cell), level), 0);
	float expected = float(terrain_cache_key(cell, level, u_terrain_cache_generation));
	if (abs(cached.b - expected) > 0.25) return false;
	// Cache storage is R=final generated height, G=macro height. Authored graph
	// displacement stays live and is applied at the exact lattice sample here.
	sample = author_procedural_lattice_sample(cell, level, vec2(cached.g, cached.r));
	return true;
}

vec2 terrain_triangle_interpolate(vec2 s00, vec2 s10, vec2 s01, vec2 s11,
		vec2 f) {
	// Static topology uses triangles (00,10,11) and (00,11,01), i.e. the diagonal
	// from the lower-left lattice vertex to the upper-right lattice vertex.
	if (f.y <= f.x) {
		return s00 * (1.0 - f.x)
			+ s10 * (f.x - f.y)
			+ s11 * f.y;
	}
	return s00 * (1.0 - f.y)
		+ s11 * f.x
		+ s01 * (f.y - f.x);
}

bool terrain_cache_fetch_triangle(vec2 lattice_cell, int level, out vec2 sample) {
	ivec2 c00 = ivec2(floor(lattice_cell));
	vec2 f = fract(lattice_cell);
	vec2 s00;
	if (max(f.x, f.y) <= 1e-5) {
		return terrain_cache_fetch_exact(c00, level, sample);
	}
	vec2 s10;
	vec2 s01;
	vec2 s11;
	if (!terrain_cache_fetch_exact(c00, level, s00)
			|| !terrain_cache_fetch_exact(c00 + ivec2(1, 0), level, s10)
			|| !terrain_cache_fetch_exact(c00 + ivec2(0, 1), level, s01)
			|| !terrain_cache_fetch_exact(c00 + ivec2(1, 1), level, s11)) {
		sample = vec2(0.0);
		return false;
	}
	sample = terrain_triangle_interpolate(s00, s10, s01, s11, f);
	return true;
}

bool terrain_cache_fetch_bilinear(vec2 lattice_cell, int level, out vec2 sample) {
	ivec2 c00 = ivec2(floor(lattice_cell));
	vec2 f = fract(lattice_cell);
	vec2 s00;
	if (max(f.x, f.y) <= 1e-5) {
		return terrain_cache_fetch_exact(c00, level, sample);
	}
	vec2 s10;
	vec2 s01;
	vec2 s11;
	if (!terrain_cache_fetch_exact(c00, level, s00)
			|| !terrain_cache_fetch_exact(c00 + ivec2(1, 0), level, s10)
			|| !terrain_cache_fetch_exact(c00 + ivec2(0, 1), level, s01)
			|| !terrain_cache_fetch_exact(c00 + ivec2(1, 1), level, s11)) {
		sample = vec2(0.0);
		return false;
	}
	sample = mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);
	return true;
}

vec2 terrain_analytic_vertex_sample(ivec2 cell, int level) {
	float spacing = ring_spacing(level);
	vec2 offset_m = vec2(cell) * spacing;
	vec3 d = direction_for_basis_offset(
		u_anchor_dir, u_anchor_right, u_anchor_up, offset_m);
	return author_procedural_lattice_sample(cell, level, terrain_sample_for_level(d, level));
}

vec2 terrain_analytic_triangle_sample(vec2 lattice_cell, int level) {
	ivec2 c00 = ivec2(floor(lattice_cell));
	vec2 f = fract(lattice_cell);
	vec2 s00 = terrain_analytic_vertex_sample(c00, level);
	if (max(f.x, f.y) <= 1e-5) return s00;
	vec2 s10 = terrain_analytic_vertex_sample(c00 + ivec2(1, 0), level);
	vec2 s01 = terrain_analytic_vertex_sample(c00 + ivec2(0, 1), level);
	vec2 s11 = terrain_analytic_vertex_sample(c00 + ivec2(1, 1), level);
	return terrain_triangle_interpolate(s00, s10, s01, s11, f);
}

vec2 terrain_cached_level_sample(vec3 d, int level, vec2 anchor_offset_m,
		bool stable_displacement) {
	if (stable_displacement) {
		vec2 lattice_cell = anchor_offset_m / max(ring_spacing(level), 1e-6);
		if (u_terrain_cache_ready > 0.5) {
			vec2 cached;
			if (terrain_cache_fetch_triangle(lattice_cell, level, cached)) return cached;
		}
		// A cold cache still has to target the same parent triangle surface. Evaluating
		// the analytic field directly at this in-between point would reintroduce the
		// same T-junction crack until the cache finished warming.
		return terrain_analytic_triangle_sample(lattice_cell, level);
	}
	return author_procedural_sample(d, level, terrain_sample_for_level(d, level));
}

vec2 terrain_cached_sample_with_morph(vec3 d, int level, float morph,
		vec2 anchor_offset_m, bool stable_displacement) {
	vec2 fine = terrain_cached_level_sample(
		d, level, anchor_offset_m, stable_displacement);
	if (level < ASTERRA_MAX_LEVEL && morph > 0.001) {
		vec2 coarse = terrain_cached_level_sample(
			d, level + 1, anchor_offset_m, stable_displacement);
		fine = mix(fine, coarse, morph);
	}
	return fine;
}

// Normals are intentionally smoother than the piecewise-planar geometry. They can
// retain bilinear cache interpolation without affecting vertex watertightness.
// If a neighboring cache sample is still warming, use its inexpensive macro
// surface; fragment derivatives retain the actual rendered triangle slope.
vec2 terrain_cached_or_macro_level(vec3 d, int level, vec2 anchor_offset_m) {
	if (u_terrain_cache_ready > 0.5) {
		vec2 cached;
		vec2 lattice_cell = anchor_offset_m / max(ring_spacing(level), 1e-6);
		if (terrain_cache_fetch_bilinear(lattice_cell, level, cached)) return cached;
	}
	float macro_h = macro_height(d, level);
	return author_procedural_sample(d, level, vec2(macro_h, macro_h));
}

vec2 terrain_cached_or_macro_with_morph(vec3 d, int level, float morph,
		vec2 anchor_offset_m) {
	vec2 fine = terrain_cached_or_macro_level(d, level, anchor_offset_m);
	if (level < ASTERRA_MAX_LEVEL && morph > 0.001) {
		fine = mix(fine,
			terrain_cached_or_macro_level(d, level + 1, anchor_offset_m), morph);
	}
	return fine;
}

vec3 terrain_cached_surface_normal(vec3 d, int level, float morph,
		float center_height, vec2 anchor_offset_m) {
	float sample_m = max(NORMAL_SAMPLE_MIN_M,
		ring_spacing(level) * NORMAL_SAMPLE_SPACING_MULT);
	vec2 offset_x = anchor_offset_m + vec2(sample_m, 0.0);
	vec2 offset_y = anchor_offset_m + vec2(0.0, sample_m);
	vec3 dx = direction_for_basis_offset(
		u_anchor_dir, u_anchor_right, u_anchor_up, offset_x);
	vec3 dy = direction_for_basis_offset(
		u_anchor_dir, u_anchor_right, u_anchor_up, offset_y);
	float hx = terrain_cached_or_macro_with_morph(dx, level, morph, offset_x).y;
	float hy = terrain_cached_or_macro_with_morph(dy, level, morph, offset_y).y;
	vec3 p0 = d * (u_planet_radius + center_height);
	vec3 px = dx * (u_planet_radius + hx);
	vec3 py = dy * (u_planet_radius + hy);
	vec3 n_raw = cross(px - p0, py - p0);
	vec3 n = length(n_raw) > 1e-8 ? normalize(n_raw) : d;
	if (dot(n, d) < 0.0) n = -n;
	return n;
}
