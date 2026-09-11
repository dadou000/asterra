// Lightweight, VERTEX-SHADER-SAFE subset of terrain_biome_texture.gdshaderinc:
// just enough of an authored texture band's own height/slope/noise gating to
// let OTHER systems place things only where a specific band is active,
// without pulling in triplanar texture sampling (which needs the full
// terrain_biome_texture.gdshaderinc + gpu_surface_pbr.gdshaderinc chain this
// file deliberately avoids so it stays cheap and dependency-free enough for a
// vertex shader). Currently used by gpu_scatter_common.gdshaderinc so a
// scatter family can be gated by "wherever I painted band N of biome B", not
// just the classifier's raw continuous material weights.
//
// Split out from terrain_biome_texture.gdshaderinc (rather than each
// declaring its own copy of the layer uniform arrays) so the SAME authored
// band data drives both the real terrain material and any scatter/other
// system reading it -- one source of truth, no duplicate uniform
// declarations if both end up in the same shader (they do, via
// terrain_biome_texture.gdshaderinc's own #include of this file).
//
// Deliberately excludes cavity: that axis needs screen-space derivatives
// (dFdx/dFdy) that do not exist in a vertex shader, so a cavity-gated band's
// mask weight here ignores its cavity range (reads as if wide-open) --
// an acceptable simplification for a placement gate, unlike the real
// fragment-shader material blend which does honor cavity.

const int BIOME_TEX_LAYER_MAX = 16;
uniform int u_biome_tex_layer_count = 0;
uniform vec4 u_biome_tex_layer_a[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_b[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_d[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_f[BIOME_TEX_LAYER_MAX];

// Identical hash to terrain_biome_profile.gdshaderinc's bp_hash, deliberately
// duplicated rather than shared -- see terrain_biome_texture.gdshaderinc's own
// header comment on btex_hash for why (no include guards, both end up
// included together in the terrain fragment shader).
float btex_hash(ivec3 p, int seed) {
	uint h = uint(p.x) * 374761393u + uint(p.y) * 668265263u + uint(p.z) * 2147483647u + uint(seed) * 1274126177u;
	h = (h ^ (h >> 13u)) * 1274126177u;
	h ^= h >> 16u;
	return float(h & 0x00ffffffu) / 16777215.0 * 2.0 - 1.0;
}

// Cosmetic patchiness only, never a height -- see terrain_biome_texture
// .gdshaderinc's own btex_noise header comment.
float btex_noise(vec3 direction, float frequency, int seed) {
	vec3 p = direction * frequency;
	ivec3 i = ivec3(floor(p));
	vec3 f = fract(p);
	f = f * f * (vec3(3.0) - 2.0 * f);
	float a0 = mix(btex_hash(i, seed), btex_hash(i + ivec3(1, 0, 0), seed), f.x);
	float a1 = mix(btex_hash(i + ivec3(0, 1, 0), seed), btex_hash(i + ivec3(1, 1, 0), seed), f.x);
	float b0 = mix(btex_hash(i + ivec3(0, 0, 1), seed), btex_hash(i + ivec3(1, 0, 1), seed), f.x);
	float b1 = mix(btex_hash(i + ivec3(0, 1, 1), seed), btex_hash(i + ivec3(1, 1, 1), seed), f.x);
	return mix(mix(a0, a1, f.y), mix(b0, b1, f.y), f.z);
}

// Smooth membership of `value` in [lo, hi], softened by `softness` on both
// edges -- see terrain_biome_texture.gdshaderinc's own btex_band.
float btex_band(float value, float lo, float hi, float softness) {
	float s = max(softness, 1e-4);
	return smoothstep(lo - s, lo, value) * (1.0 - smoothstep(hi, hi + s, value));
}

// See header comment. `dir` only needs to be a stable per-candidate direction
// for the noise term (matches asterra_biome_texture_apply's own `d`).
float asterra_biome_texture_mask_weight(int biome, int layer_index, vec3 dir,
		float height_m, float local_baseline_m, float slope_deg) {
	if (layer_index < 0 || layer_index >= u_biome_tex_layer_count) {
		return 0.0;
	}
	vec4 a = u_biome_tex_layer_a[layer_index];
	if (int(floor(a.x + 0.5)) != biome) {
		return 0.0;
	}
	vec4 b = u_biome_tex_layer_b[layer_index];
	vec4 f = u_biome_tex_layer_f[layer_index];
	int flags = int(round(f.w));
	float effective_height_m = (flags & 16) != 0 ? (height_m - local_baseline_m) : height_m;
	float w = btex_band(effective_height_m, a.z, a.w, b.z) * btex_band(slope_deg, b.x, b.y, 2.0);
	if (w <= 0.0001) {
		return 0.0;
	}
	vec4 dd = u_biome_tex_layer_d[layer_index];
	float noise_scale = dd.x;
	float noise_strength = clamp(dd.y, 0.0, 1.0);
	if (noise_scale > 0.0 && noise_strength > 0.0) {
		int seed = int(floor(b.w + 0.5));
		float patch01 = btex_noise(normalize(dir), noise_scale, seed) * 0.5 + 0.5;
		w *= mix(1.0, smoothstep(0.3, 0.7, patch01), noise_strength);
	}
	return w;
}
