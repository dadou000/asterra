// Biome Texture: per-biome surface-LOOK composition, the material-authoring
// counterpart to terrain_biome_profile.gdshaderinc's per-biome HEIGHT
// composition. An ordered stack of layers ("bands"), each a height range
// AND/OR a slope range (either can be left wide-open to ignore it), an
// appearance (a flat colour, a colour GRADIENT, one of the game's existing
// tiled/mipmapped ground materials, or a user-imported PBR texture -- any of
// which can be tinted by the layer's colour), full PBR material control
// (roughness/metallic/anisotropy/emission, each independently opt-in so a
// band can just recolour without also flattening every other material
// property), and an optional random "patchiness" so a band edge reads as
// scattered and organic rather than a hard, surveyed line.
//
// Purely a visual (albedo/roughness/metallic/anisotropy/emission) overlay: it
// has no CPU mirror, unlike Biome Terrain's height layers, because surface
// look does not feed contact/AGL.
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
// The built-in "assign different texture" choices reuse
// gpu_surface_pbr.gdshaderinc's existing Ground/Grass/Mud/Forest triplanar
// materials rather than adding a new texture-array uniform for them: those
// are already tiled, normal-mapped, and sampled with textureGrad() against
// real mip chains (automatic mipmapping, for free). User-imported PBR
// textures (albedo + optional roughness, embedded as PNG bytes at authoring
// time -- see TerrainDisplacementRuntime's CUSTOM_TEXTURE node handling) are
// stored in their own small sampler2DArray pair and triplanar-sampled with
// the SAME shared math (spbr_weights/spbr_axis_sign/spbr_uv_*/spbr_grad_* --
// generic over which sampler they read, so no new triplanar code needed for
// the custom case either). Normal maps are not supported for imported
// textures in this first version (only for the four built-ins, which already
// had them) -- a plausible follow-up, not a limitation of the packing.
//
// Because the composited result here is applied to the same
// `albedo`/`roughness`/`metallic`/`anisotropy` the rest of the fragment
// shader already carries forward (see asterra_biome_texture_apply's call
// site), "micro geometry tessellation" -- the existing near-camera relief
// bump system (gpu_material_microrelief.gdshaderinc's mmr_height, blended
// via v_micro_layer) -- composes with it automatically: that system perturbs
// the shading normal and adds fine colour variation on top of whatever
// albedo it is handed, biome-textured or not, with no special-casing needed
// here. Emission is additive (a separate accumulator the caller adds into
// its own EMISSION at the very end), since it represents light the surface
// itself gives off (lava, glowing crystal, bioluminescence) rather than
// something reflected sunlight/skylight already accounts for.
//
// Packing per layer (mirrored by TerrainDisplacementRuntime on the CPU, for
// authoring/persistence only -- there is no runtime CPU evaluation):
//   a = (biome_id, texture_choice, height_min_m, height_max_m)
//   b = (slope_min_deg, slope_max_deg, edge_softness_m, seed)
//   c = (color_a.r, color_a.g, color_a.b, opacity)
//   d = (noise_scale, noise_strength, tint_strength, custom_texture_index)
//   e = (color_b.r, color_b.g, color_b.b, gradient_strength)
//   f = (roughness_value, metallic_value, anisotropy_value, enable_flags)
//   g = (emission.r, emission.g, emission.b, emission_strength)
// texture_choice: 0 flat/gradient colour, 1 ground, 2 grass, 3 mud, 4 forest,
// 5 a user-imported texture (selected by custom_texture_index into the
// library built by TerrainDisplacementRuntime; -1 = none, treated as choice 0).
// height_min_m/height_max_m of -1e6/1e6 and slope_min_deg/slope_max_deg of
// 0/180 are the "wide open, ignore this axis" sentinels an authored layer
// uses when it only cares about the other one. gradient_strength blends from
// a flat colour_a (0) to a full colour_a->colour_b sweep across the band's
// own height range (1) -- see the "height dependent colour map" comment on
// asterra_biome_texture_apply. enable_flags bit 0/1/2/3 = roughness/
// metallic/anisotropy/emission override active for this layer; a control
// left off leaves the corresponding output exactly as the layers under it
// (or the terrain's base material) left it, rather than forcing every band
// to also flatten roughness just to recolour something.

const int BIOME_TEX_LAYER_MAX = 16;
uniform int u_biome_tex_layer_count = 0;
uniform vec4 u_biome_tex_layer_a[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_b[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_c[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_d[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_e[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_f[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_g[BIOME_TEX_LAYER_MAX];

// User-imported PBR textures (albedo + optional roughness), built once by
// TerrainDisplacementRuntime from embedded PNG bytes into fixed-size array
// layers (Texture2DArray layers must share one resolution) whenever the
// authored library changes -- not a per-frame cost. u_biome_tex_custom_tile_m
// is the world-space tile size (metres) each slot was authored with.
const int BIOME_TEX_CUSTOM_MAX = 8;
uniform int u_biome_tex_custom_count = 0;
uniform sampler2DArray u_biome_tex_custom_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2DArray u_biome_tex_custom_roughness : filter_linear_mipmap_anisotropic, repeat_enable;
uniform float u_biome_tex_custom_tile_m[BIOME_TEX_CUSTOM_MAX];

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

// Generic triplanar sample of ANY sampler2DArray layer, reusing
// gpu_surface_pbr.gdshaderinc's shared weight/UV/gradient helpers (they take
// a tile size and an axis sign, not a specific sampler, so the same math
// serves the built-in ground/grass/mud/forest samplers and this one).
vec3 btex_custom_albedo(int idx, vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	if (idx < 0 || idx >= u_biome_tex_custom_count) {
		return vec3(1.0);
	}
	float tile_m = u_biome_tex_custom_tile_m[idx];
	float layer = float(idx);
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	return textureGrad(u_biome_tex_custom_albedo, vec3(spbr_uv_x(p, tile_m, s.x), layer),
			spbr_grad_x(dpdx, tile_m, s.x), spbr_grad_x(dpdy, tile_m, s.x)).rgb * w.x
		+ textureGrad(u_biome_tex_custom_albedo, vec3(spbr_uv_y(p, tile_m, s.y), layer),
			spbr_grad_y(dpdx, tile_m, s.y), spbr_grad_y(dpdy, tile_m, s.y)).rgb * w.y
		+ textureGrad(u_biome_tex_custom_albedo, vec3(spbr_uv_z(p, tile_m, s.z), layer),
			spbr_grad_z(dpdx, tile_m, s.z), spbr_grad_z(dpdy, tile_m, s.z)).rgb * w.z;
}

float btex_custom_rough(int idx, vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	if (idx < 0 || idx >= u_biome_tex_custom_count) {
		return 0.9;
	}
	float tile_m = u_biome_tex_custom_tile_m[idx];
	float layer = float(idx);
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	return textureGrad(u_biome_tex_custom_roughness, vec3(spbr_uv_x(p, tile_m, s.x), layer),
			spbr_grad_x(dpdx, tile_m, s.x), spbr_grad_x(dpdy, tile_m, s.x)).r * w.x
		+ textureGrad(u_biome_tex_custom_roughness, vec3(spbr_uv_y(p, tile_m, s.y), layer),
			spbr_grad_y(dpdx, tile_m, s.y), spbr_grad_y(dpdy, tile_m, s.y)).r * w.y
		+ textureGrad(u_biome_tex_custom_roughness, vec3(spbr_uv_z(p, tile_m, s.z), layer),
			spbr_grad_z(dpdx, tile_m, s.z), spbr_grad_z(dpdy, tile_m, s.z)).r * w.z;
}

// Smooth membership of `value` in [lo, hi], softened by `softness` on both
// edges so a band never reads as a surveyed, hard-edged line.
float btex_band(float value, float lo, float hi, float softness) {
	float s = max(softness, 1e-4);
	return smoothstep(lo - s, lo, value) * (1.0 - smoothstep(hi, hi + s, value));
}

// Composes every layer bound to `biome` on top of the terrain's existing,
// already-classified/textured material, each blended in by its own
// height/slope/(optional patchiness) coverage weight. `height_m` is height
// above sea level; `slope_deg` is the angle between the shading normal and
// local "up" (0 flat, 90 vertical). `dpos`/`dpdx`/`dpdy`/`n` are the same
// precision-safe detail position/derivatives/normal
// gpu_surface_pbr.gdshaderinc's own triplanar sampling uses, so a
// texture-choice layer tiles and mipmaps identically to the rest of the
// terrain's material. `emission_add` accumulates additively (see the header
// comment) rather than blending like the others -- the caller adds it into
// EMISSION once, after this returns. Zero-cost when no layer is bound.
void asterra_biome_texture_apply(vec3 dir, int biome, float height_m, float slope_deg,
		vec3 dpos, vec3 dpdx, vec3 dpdy, vec3 n,
		inout vec3 albedo, inout float roughness, inout float metallic,
		inout float anisotropy, inout vec3 emission_add) {
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
		vec4 e = u_biome_tex_layer_e[i];
		// "Height dependent colour map": a plain lerp from colour_a to
		// colour_b across the band's OWN [height_min, height_max] range
		// (clamped, so it also behaves sanely on a slope-only band whose
		// height range is the wide-open sentinel). gradient_strength (e.a)
		// scales how much of that sweep applies -- 0 keeps a flat colour_a,
		// 1 is the full gradient, in between blends toward it.
		float grad_t = clamp((height_m - a.z) / max(a.w - a.z, 1e-4), 0.0, 1.0);
		vec3 layer_color = mix(c.rgb, e.rgb, grad_t * clamp(e.a, 0.0, 1.0));

		int choice = int(floor(a.y + 0.5));
		int custom_idx = int(round(dd.w));
		float tint = clamp(dd.z, 0.0, 1.0);
		vec3 material_albedo;
		float material_rough;
		if (choice == 0) {
			material_albedo = layer_color;
			material_rough = 0.9;
		} else if (choice == 5 && custom_idx >= 0) {
			material_albedo = mix(btex_custom_albedo(custom_idx, dpos, dpdx, dpdy, n), layer_color, tint);
			material_rough = btex_custom_rough(custom_idx, dpos, dpdx, dpdy, n);
		} else {
			material_albedo = mix(btex_material_albedo(choice, dpos, dpdx, dpdy, n), layer_color, tint);
			material_rough = btex_material_rough(choice, dpos, dpdx, dpdy, n);
		}

		float opacity = clamp(c.a, 0.0, 1.0) * w;
		albedo = mix(albedo, material_albedo, opacity);

		vec4 f = u_biome_tex_layer_f[i];
		int flags = int(round(f.w));
		// A control left off (its bit unset) leaves whatever value the
		// layers under it (or the terrain's base material) already produced
		// untouched -- so a band that only wants to recolour something does
		// not also have to decide a roughness/metallic/anisotropy value for
		// it. Roughness defaults its "as if off" contribution to the
		// texture/flat-colour's own natural roughness rather than skipping
		// the mix, so a roughness *override* (bit set) still blends smoothly
		// against a plausible neighbour instead of the terrain's -- possibly
		// very different -- prior roughness.
		float roughness_target = (flags & 1) != 0 ? f.x : material_rough;
		roughness = mix(roughness, roughness_target, opacity);
		if ((flags & 2) != 0) {
			metallic = mix(metallic, clamp(f.y, 0.0, 1.0), opacity);
		}
		if ((flags & 4) != 0) {
			anisotropy = mix(anisotropy, clamp(f.z, -1.0, 1.0), opacity);
		}
		if ((flags & 8) != 0) {
			vec4 g = u_biome_tex_layer_g[i];
			emission_add += g.rgb * max(g.a, 0.0) * opacity;
		}
	}
}
