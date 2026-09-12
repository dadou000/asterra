// Toroidal GPU terrain-cache sampling for the spherical geometry clipmap.
// Include this after the analytic height helpers; cache misses deliberately fall
// back to those helpers so staggered warm-up can never expose stale geometry.
//
// IMPORTANT: the visible clipmap is a triangle mesh, not a bilinear height patch.
// Fine vertices that morph to their parent therefore have to converge to the
// parent's actual piecewise-planar triangles. Bilinear interpolation of the four
// parent samples is a different surface between lattice vertices and creates the
// classic alternating zipper crack at every LOD boundary.

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
	#ifdef ASTERRA_AUTHORED_TERRAIN
	sample = author_procedural_lattice_sample(cell, level, vec2(cached.g, cached.r));
	#else
	sample = vec2(cached.g, cached.r);
	#endif
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
	#ifdef ASTERRA_AUTHORED_TERRAIN
	return author_procedural_lattice_sample(cell, level, terrain_sample_for_level(d, level));
	#else
	return terrain_sample_for_level(d, level);
	#endif
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
	#ifdef ASTERRA_AUTHORED_TERRAIN
	return author_procedural_sample(d, level, terrain_sample_for_level(d, level));
	#else
	return terrain_sample_for_level(d, level);
	#endif
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
	#ifdef ASTERRA_AUTHORED_TERRAIN
	return author_procedural_sample(d, level, vec2(macro_h, macro_h));
	#else
	return vec2(macro_h, macro_h);
	#endif
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

	// Central differences are intentional here. The old implementation sampled
	// only +anchor-right and +anchor-up. That produces a first-order normal bias
	// tied to the current clipmap tangent frame; when a terrain/floating-origin
	// handoff changes the anchor basis or lattice phase, the biased bright/dark side
	// moves with it. Symmetric +/- samples make the estimate translation-neutral
	// and remove that rebase-dependent directional shading term.
	vec2 offset_xp = anchor_offset_m + vec2(sample_m, 0.0);
	vec2 offset_xn = anchor_offset_m - vec2(sample_m, 0.0);
	vec2 offset_yp = anchor_offset_m + vec2(0.0, sample_m);
	vec2 offset_yn = anchor_offset_m - vec2(0.0, sample_m);

	vec3 dxp = direction_for_basis_offset(
		u_anchor_dir, u_anchor_right, u_anchor_up, offset_xp);
	vec3 dxn = direction_for_basis_offset(
		u_anchor_dir, u_anchor_right, u_anchor_up, offset_xn);
	vec3 dyp = direction_for_basis_offset(
		u_anchor_dir, u_anchor_right, u_anchor_up, offset_yp);
	vec3 dyn = direction_for_basis_offset(
		u_anchor_dir, u_anchor_right, u_anchor_up, offset_yn);

	float hxp = terrain_cached_or_macro_with_morph(dxp, level, morph, offset_xp).y;
	float hxn = terrain_cached_or_macro_with_morph(dxn, level, morph, offset_xn).y;
	float hyp = terrain_cached_or_macro_with_morph(dyp, level, morph, offset_yp).y;
	float hyn = terrain_cached_or_macro_with_morph(dyn, level, morph, offset_yn).y;

	// The visible surface composes deformation and per-biome terrain after the
	// cache sample. Include those same terms in the smooth normal so shading and
	// slope classification no longer need the triangle-face normal to reveal them.
	// Both branches are controlled by uniforms and collapse to zero work when the
	// corresponding feature is inactive.
	if (u_edit_ready > 0.5 || u_active_deform_ready > 0.5) {
		hxp += terrain_edit_delta(dxp, u_planet_radius);
		hxn += terrain_edit_delta(dxn, u_planet_radius);
		hyp += terrain_edit_delta(dyp, u_planet_radius);
		hyn += terrain_edit_delta(dyn, u_planet_radius);
	}
	if (u_biome_layer_count > 0) {
		float biome_spacing_m = ring_spacing(level);
		hxp += asterra_biome_profile_height(dxp, biome_spacing_m);
		hxn += asterra_biome_profile_height(dxn, biome_spacing_m);
		hyp += asterra_biome_profile_height(dyp, biome_spacing_m);
		hyn += asterra_biome_profile_height(dyn, biome_spacing_m);
	}

	vec3 pxp = dxp * (u_planet_radius + hxp);
	vec3 pxn = dxn * (u_planet_radius + hxn);
	vec3 pyp = dyp * (u_planet_radius + hyp);
	vec3 pyn = dyn * (u_planet_radius + hyn);
	vec3 tangent_x = pxp - pxn;
	vec3 tangent_y = pyp - pyn;
	vec3 n_raw = cross(tangent_x, tangent_y);
	vec3 n = length(n_raw) > 1e-8 ? normalize(n_raw) : d;
	if (dot(n, d) < 0.0) n = -n;
	return n;
}
