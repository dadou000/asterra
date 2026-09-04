// Biome Texture: per-biome surface-LOOK composition, the material-authoring
// counterpart to terrain_biome_profile.gdshaderinc's per-biome HEIGHT
// composition. An ordered stack of layers, each a height range AND/OR a
// slope range (either can be left wide-open to ignore it), an appearance
// (a flat colour, or one of the game's existing tiled/mipmapped ground
// materials optionally tinted by that colour), and an optional random
// "patchiness" so a band edge reads as scattered and organic rather than a
// hard, surveyed line.
//
// Purely a visual (albedo/roughness) overlay: it has no CPU mirror, unlike
// Biome Terrain's height layers, because surface look does not feed
// contact/AGL.
//
// Deliberately does NOT use the authored material bytecode VM
// (terrain_author_material_bytecode.gdshaderinc, included via
// terrain_author_material_post_cached.gdshaderinc) for the same reason Biome
// Terrain avoids the displacement VM (terrain_biome_profile.gdshaderinc's own
// header): a big surface shader recompiling around that VM's 32-slot local
// register array has triggered Vulkan device loss on current NVIDIA drivers.
// This lowers to plain uniforms and a bounded loop instead -- always safe to
// compile, and free (one branch) whenever u_biome_tex_layer_count is 0.
//
// "Assign different texture" reuses gpu_surface_pbr.gdshaderinc's existing
// Ground/Grass/Mud/Forest triplanar materials rather than adding a new
// texture-array/atlas uniform: those are already tiled, normal-mapped, and
// sampled with textureGrad() against real mip chains (automatic mipmapping,
// for free), so a layer just picks which one to blend in -- see
// btex_material_albedo/btex_material_rough. Because the composited result
// here is applied to the same `albedo` the rest of the fragment shader
// already carries forward (see asterra_biome_texture_apply's call site),
// "micro geometry tessellation" -- the existing near-camera relief bump
// system (gpu_material_microrelief.gdshaderinc's mmr_height, blended via
// v_micro_layer) -- composes with it automatically: that system perturbs the
// shading normal and adds fine colour variation on top of whatever albedo it
// is handed, biome-textured or not, with no special-casing needed here.
//
// Packing per layer (mirrored by TerrainDisplacementRuntime on the CPU, for
// authoring/persistence only -- there is no runtime CPU evaluation):
//   a = (biome_id, texture_choice, height_min_m, height_max_m)
//   b = (slope_min_deg, slope_max_deg, edge_softness_m, seed)
//   c = (color.r, color.g, color.b, opacity)
//   d = (noise_scale, noise_strength, tint_strength, 0)
// texture_choice: 0 flat colour, 1 ground, 2 grass, 3 mud, 4 forest.
// height_min_m/height_max_m of -1e6/1e6 and slope_min_deg/slope_max_deg of
// 0/180 are the "wide open, ignore this axis" sentinels an authored layer
// uses when it only cares about the other one.

const int BIOME_TEX_LAYER_MAX = 16;
uniform int u_biome_tex_layer_count = 0;
uniform vec4 u_biome_tex_layer_a[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_b[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_c[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_d[BIOME_TEX_LAYER_MAX];

// Identical hash to terrain_biome_profile.gdshaderinc's bp_hash, deliberately
// duplicated rather than shared: that file is not #included here (both end
// up included together in the same fragment shader, and neither has an
// include guard, so a shared #include would double-declare every uniform and
// function in it and fail to compile).
float btex_hash(ivec3 p, int seed) {
	uint h = uint(p.x) * 374761393u + uint(p.y) * 668265263u + uint(p.z) * 2147483647u + uint(seed) * 1274126177u;
	h = (h ^ (h >> 13u)) * 1274126177u;
	h ^= h >> 16u;
	return float(h & 0x00ffffffu) / 16777215.0 * 2.0 - 1.0;
}

// Cosmetic patchiness only, never a height -- unlike bp_noise this skips the
// FMA precision correction and lattice-wrap carry fix those needed to avoid
// spiking displaced GEOMETRY at high frequency. Worst case here a coarse
// fract() step mildly stipples a texture-choice boundary; nothing like that
// is at stake for a colour/material blend weight.
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

vec3 btex_material_albedo(int choice, vec3 dpos, vec3 dpdx, vec3 dpdy, vec3 n) {
	if (choice == 1) return spbr_tri_albedo_ground(dpos, dpdx, dpdy, n);
	if (choice == 2) return spbr_tri_albedo_grass(dpos, dpdx, dpdy, n);
	if (choice == 3) return spbr_tri_albedo_mud(dpos, dpdx, dpdy, n);
	if (choice == 4) return spbr_tri_albedo_forest(dpos, dpdx, dpdy, n);
	return vec3(1.0);
}

float btex_material_rough(int choice, vec3 dpos, vec3 dpdx, vec3 dpdy, vec3 n) {
	if (choice == 1) return spbr_tri_rough_ground(dpos, dpdx, dpdy, n);
	if (choice == 2) return spbr_tri_rough_grass(dpos, dpdx, dpdy, n);
	if (choice == 3) return spbr_tri_rough_mud(dpos, dpdx, dpdy, n);
	if (choice == 4) return spbr_tri_rough_forest(dpos, dpdx, dpdy, n);
	return 0.9; // a flat colour reads as a plain matte surface by default
}

// Smooth membership of `value` in [lo, hi], softened by `softness` on both
// edges so a band never reads as a surveyed, hard-edged line.
float btex_band(float value, float lo, float hi, float softness) {
	float s = max(softness, 1e-4);
	return smoothstep(lo - s, lo, value) * (1.0 - smoothstep(hi, hi + s, value));
}

// Composes every layer bound to `biome` on top of `albedo`/`roughness` (the
// terrain's existing, already-classified/textured look), each blended in by
// its own height/slope/(optional patchiness) coverage weight. `height_m` is
// height above sea level; `slope_deg` is the angle between the shading
// normal and local "up" (0 flat, 90 vertical). `dpos`/`dpdx`/`dpdy`/`n` are
// the same precision-safe detail position/derivatives/normal
// gpu_surface_pbr.gdshaderinc's own triplanar sampling uses, so a
// texture-choice layer tiles and mipmaps identically to the rest of the
// terrain's material. Zero-cost when no layer is bound.
void asterra_biome_texture_apply(vec3 dir, int biome, float height_m, float slope_deg,
		vec3 dpos, vec3 dpdx, vec3 dpdy, vec3 n,
		inout vec3 albedo, inout float roughness) {
	if (u_biome_tex_layer_count <= 0) {
		return;
	}
	vec3 d = normalize(dir);
	for (int i = 0; i < BIOME_TEX_LAYER_MAX; i++) {
		if (i >= u_biome_tex_layer_count) {
			break;
		}
		vec4 a = u_biome_tex_layer_a[i];
		if (int(floor(a.x + 0.5)) != biome) {
			continue;
		}
		vec4 b = u_biome_tex_layer_b[i];
		float w = btex_band(height_m, a.z, a.w, b.z) * btex_band(slope_deg, b.x, b.y, 2.0);
		if (w <= 0.0001) {
			continue;
		}
		vec4 dd = u_biome_tex_layer_d[i];
		float noise_scale = dd.x;
		float noise_strength = clamp(dd.y, 0.0, 1.0);
		if (noise_scale > 0.0 && noise_strength > 0.0) {
			int seed = int(floor(b.w + 0.5));
			// Turns the band's smooth coverage into a scattered/dappled one
			// as strength increases: at strength 0 this is a no-op (w
			// unchanged); at strength 1, w is replaced by a soft-edged,
			// roughly-50%-coverage noise mask, the "spread randomly across
			// the height so it looks decent" look.
			float patch01 = btex_noise(d, noise_scale, seed) * 0.5 + 0.5;
			w *= mix(1.0, smoothstep(0.3, 0.7, patch01), noise_strength);
		}
		if (w <= 0.0001) {
			continue;
		}
		vec4 c = u_biome_tex_layer_c[i];
		int choice = int(floor(a.y + 0.5));
		vec3 layer_color = c.rgb;
		float tint = clamp(dd.z, 0.0, 1.0);
		vec3 material_albedo = choice == 0 ? layer_color
			: mix(btex_material_albedo(choice, dpos, dpdx, dpdy, n), layer_color, tint);
		float material_rough = btex_material_rough(choice, dpos, dpdx, dpdy, n);
		float opacity = clamp(c.a, 0.0, 1.0) * w;
		albedo = mix(albedo, material_albedo, opacity);
		roughness = mix(roughness, material_rough, opacity);
	}
}
