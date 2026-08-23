// Close-range ground material synthesis.
//
// Two problems are solved here, and the first one has to be solved before the
// second is even possible.
//
// PRECISION. Planet-space positions are of order 1e6 m. A float32 holds about
// seven digits, so at that magnitude the smallest representable step is ~6 cm --
// and `v_planet` is built by adding a small local offset to a float32 origin,
// which throws the local precision away entirely. Any noise evaluated on it is
// therefore sampling a lattice quantised to 6 cm, which is exactly why the
// finest existing octave came out as stripes instead of texture. Everything in
// here runs on `v_render` (the position relative to the floating origin, which
// is small and exact) plus a CPU-supplied origin offset already reduced modulo
// GD_ORIGIN_WRAP in double precision. The sum is planet-anchored, survives a
// rebase unchanged, and holds sub-millimetre precision.
//
// DETAIL ACROSS SCALE. Fixed octaves cannot serve both a 2 m view and orbit: the
// fine ones alias into shimmer at distance and the coarse ones are all that is
// left up close. The fBm here instead chooses its octave range from the pixel
// footprint, fading out any octave finer than the pixel being drawn. The surface
// then carries detail at whatever resolution it is currently being viewed at,
// which is what "detailed at every altitude" actually requires.

/// Reduction modulus for the CPU-supplied origin, metres. Detail therefore
/// repeats every 4096 m -- invisible, because seeing the repeat would mean
/// resolving centimetres across four kilometres at once.
const float GD_ORIGIN_WRAP = 4096.0;

// ------------------------------------------------------------------ hashing ---
// Integer hashing rather than the usual fract(sin(...)) chain. On a lattice the
// input is exactly an integer, so an integer hash has no precision behaviour to
// degrade at all -- which is the whole point after the discussion above.
uint gd_hash_u(uvec3 p) {
	p *= uvec3(374761393u, 668265263u, 2147483647u);
	uint h = (p.x ^ p.y ^ p.z) * 1274126177u;
	h ^= h >> 13u;
	h *= 1274126177u;
	h ^= h >> 16u;
	return h;
}

float gd_hash(vec3 lattice) {
	uvec3 u = uvec3(ivec3(lattice) + ivec3(1048576));
	return float(gd_hash_u(u) & 0xFFFFFFu) / 16777216.0;
}

vec3 gd_hash3(vec3 lattice) {
	uvec3 u = uvec3(ivec3(lattice) + ivec3(1048576));
	uint h = gd_hash_u(u);
	return vec3(float(h & 0x3FFu), float((h >> 10u) & 0x3FFu),
		float((h >> 20u) & 0x3FFu)) / 1023.0;
}

float gd_value(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(
		mix(mix(gd_hash(i), gd_hash(i + vec3(1.0, 0.0, 0.0)), f.x),
			mix(gd_hash(i + vec3(0.0, 1.0, 0.0)), gd_hash(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
		mix(mix(gd_hash(i + vec3(0.0, 0.0, 1.0)), gd_hash(i + vec3(1.0, 0.0, 1.0)), f.x),
			mix(gd_hash(i + vec3(0.0, 1.0, 1.0)), gd_hash(i + vec3(1.0, 1.0, 1.0)), f.x), f.y),
		f.z);
}

// ------------------------------------------------------------------- fields ---
/// How much an octave of this wavelength should contribute at this pixel
/// footprint.
///
/// The cut is at several pixels per lattice cell, not one. Value noise is
/// defined on a lattice and interpolated between its corners, so near Nyquist
/// the lattice itself is what you see -- axis-aligned, and especially obvious
/// once anything downstream thresholds it, which turns the visible lattice into
/// stair-stepped edges. Nyquist is the wrong bar here: an octave needs to be
/// oversampled before it reads as noise rather than as a grid.
///
/// It does not need to be oversampled ninefold, though, and it was. Combined
/// with a pixel angle hardcoded for a 900-line window, the finest octave a 4K
/// frame ever saw was about twenty-five screen pixels across -- so the extra
/// resolution bought nothing but a larger blur. Measured at 4K against a warped,
/// many-octave field, five and a half pixels per cell shows no lattice and
/// carries visibly more grain.
float gd_octave_weight(float wl, float pixel_m) {
	return smoothstep(pixel_m * 2.0, pixel_m * 5.5, wl);
}

/// Scale-aware fBm between two wavelengths. `wl_max` is the coarsest octave in
/// metres and `wl_min` the finest worth generating; octaves finer than the pixel
/// footprint fade out instead of aliasing. Returns roughly -0.5 to 0.5.
float gd_fbm_band(vec3 p, float wl_max, float wl_min, float pixel_m, float gain) {
	float amp = 1.0;
	float total = 0.0;
	float norm = 1e-5;
	float wl = wl_max;
	for (int i = 0; i < 7; i++) {
		if (wl < wl_min) break;
		float w = gd_octave_weight(wl, pixel_m);
		if (w > 0.002) {
			total += (gd_value(p / wl) - 0.5) * amp * w;
			norm += amp * w;
		}
		amp *= gain;
		wl *= 0.5;
	}
	return total / norm * 0.5;
}

float gd_fbm(vec3 p, float wavelength, float pixel_m, float gain) {
	return gd_fbm_band(p, wavelength, 0.0, pixel_m, gain);
}

/// Ridged variant. Vegetation and erosion both produce creased fields rather
/// than smooth ones, and a plain fBm is the main reason procedural ground reads
/// as blurred cloud.
float gd_ridged(vec3 p, float wavelength, float pixel_m) {
	float amp = 1.0;
	float total = 0.0;
	float norm = 1e-5;
	float wl = wavelength;
	for (int i = 0; i < 5; i++) {
		float w = gd_octave_weight(wl, pixel_m);
		if (w > 0.002) {
			float n = gd_value(p / wl) * 2.0 - 1.0;
			total += (1.0 - abs(n)) * amp * w;
			norm += amp * w;
		}
		amp *= 0.55;
		wl *= 0.5;
	}
	return total / norm;
}

/// Polynomial smooth minimum.
float gd_smin(float a, float b, float k) {
	float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
	return mix(b, a, h) - k * h * (1.0 - h);
}

/// Cheap cellular field. Returns distance to the nearest jittered site, roughly
/// 0 at a site centre and 1 between sites. This is what gives clumped
/// vegetation, gravel and soil aggregates their discrete character; noise alone
/// only ever produces a continuous blur.
///
/// The minimum is smoothed rather than taken outright. A hard min is continuous
/// in value but not in gradient -- the derivative jumps wherever the nearest
/// site changes -- and because this same field drives a derivative-based bump,
/// those jumps arrive on screen as a hard crease along every cell boundary,
/// which reads as blocky stair-stepped edges rather than as clumps.
float gd_cells(vec3 p) {
	vec3 i = floor(p);
	vec3 f = p - i;
	float best = 8.0;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				vec3 o = vec3(float(x), float(y), float(z));
				vec3 site = o + gd_hash3(i + o) * 0.92 + 0.04;
				vec3 d = site - f;
				// k is applied to *squared* distance, so it has to be small; at the scale of
				// the distances themselves it drives whole cells to the clamp and turns
				// the field into flat plateaus with lattice-shaped borders.
				best = gd_smin(best, dot(d, d), 0.05);
			}
		}
	}
	return clamp(sqrt(max(best, 0.0)), 0.0, 1.0);
}

/// Tangent frame on the sphere, used to give ground structure a direction.
void gd_basis(vec3 up, out vec3 t, out vec3 b) {
	vec3 seed = abs(up.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	t = normalize(cross(seed, up));
	b = cross(up, t);
}

// ------------------------------------------------------------------ shading ---
/// Perturb a normal by the gradient of a scalar field, using screen derivatives.
/// The field is already being evaluated for albedo, so its bump costs two extra
/// interpolations rather than another set of noise samples.
vec3 gd_perturb_normal(vec3 world_pos, vec3 n, float height_signal, float strength) {
	vec3 dpdx = dFdx(world_pos);
	vec3 dpdy = dFdy(world_pos);
	float dhdx = dFdx(height_signal);
	float dhdy = dFdy(height_signal);
	vec3 r1 = cross(dpdy, n);
	vec3 r2 = cross(n, dpdx);
	float det = dot(dpdx, r1);
	float orient = det < 0.0 ? -1.0 : 1.0;
	vec3 grad = orient * (r1 * dhdx + r2 * dhdy);
	return normalize(abs(det) * n - grad * strength);
}
