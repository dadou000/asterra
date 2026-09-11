// Biome Texture: per-biome surface-LOOK composition, the material-authoring
// counterpart to terrain_biome_profile.gdshaderinc's per-biome HEIGHT
// composition. An ordered stack of layers ("bands"), each a height range
// (absolute sea-level OR relative to the local terrain's own broad trend),
// a slope range, and a cavity/convexity range (crevices vs. ridges) --
// any of which can be left wide-open to ignore it -- an appearance (a flat
// colour, a colour GRADIENT, one of the game's existing tiled/mipmapped
// ground materials, or a user-imported PBR texture -- any of which can be
// tinted by the layer's colour), full PBR material control (roughness/
// metallic/anisotropy/emission, each independently opt-in so a band can just
// recolour without also flattening every other material property), and an
// optional random "patchiness" so a band edge reads as scattered and organic
// rather than a hard, surveyed line.
//
// Purely a visual (albedo/normal/roughness/metallic/anisotropy/emission)
// overlay: it has no CPU mirror, unlike Biome Terrain's height layers,
// because surface look does not feed contact/AGL.
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
// textures (albedo + optional roughness + optional normal map, embedded as
// PNG bytes at authoring time -- see TerrainDisplacementRuntime's
// CUSTOM_TEXTURE node handling) are stored in their own small
// sampler2DArray triplet and triplanar-sampled with the SAME shared math
// (spbr_weights/spbr_axis_sign/spbr_uv_*/spbr_grad_*/spbr_normal_world --
// generic over which sampler they read, so no new triplanar code needed for
// the custom case either, just the same tangent-space-to-world conversion
// spbr_tri_normal_ground and friends already use). A missing roughness/
// normal image defaults to a neutral fill (flat 0.9 rough / flat tangent-
// space "up" normal) rather than reusing the albedo image, so an unauthored
// channel doesn't silently inherit albedo colour variation.
//
// Every layer's natural material normal (the built-in's own normal map, the
// imported texture's if provided, or the unperturbed geometric normal for a
// flat colour) blends into the shading normal the same way roughness
// blends in below: unconditionally, by the layer's own opacity, no separate
// enable flag needed -- a texture always "has" a normal (even if that's just
// "unperturbed"), the same way it always "has" a roughness.
//
// Because the composited result here is applied to the same
// `albedo`/`roughness`/`metallic`/`anisotropy`/shading-normal the rest of
// the fragment shader already carries forward (see
// asterra_biome_texture_apply's call site), "micro geometry tessellation" --
// the existing near-camera relief bump system
// (gpu_material_microrelief.gdshaderinc's mmr_height, blended via
// v_micro_layer) -- composes with it automatically: that system perturbs the
// shading normal and adds fine colour variation on top of whatever albedo it
// is handed, biome-textured or not, with no special-casing needed here.
// Emission is additive (a separate accumulator the caller adds into its own
// EMISSION at the very end), since it represents light the surface itself
// gives off (lava, glowing crystal, bioluminescence) rather than something
// reflected sunlight/skylight already accounts for.
//
// Height reference (absolute vs. relative): height_m is normally metres
// above sea level, but a layer can instead ask to be measured relative to
// `local_baseline_m` -- the terrain's own broad-scale trend at this point
// (the same low-frequency "macro elevation" the renderer already samples
// with mips for continental/regional shaping, passed in by the caller, see
// spherical_geometry_clipmap_cached_surface.gdshader's v_macro_h). This is
// what lets a band mean "the top 50m of whatever hill this is" instead of
// "everything above 50m absolute" -- the former tracks the hill wherever it
// sits, the latter doesn't.
//
// Cavity/convexity: a cheap, distance-stable curvature estimate from the
// screen-space derivatives of the shading normal and position (divided by
// the squared position derivative to cancel out the pixel footprint, which
// is what makes it stable across camera distance/LOD rather than screen-
// resolution-dependent) -- see btex_cavity_value. Negative = convex (a
// ridge/edge catches this), positive = concave (a crevice/crack catches
// this). It is an approximation of true surface curvature, not a baked
// value, so it can read a little differently on very high-frequency
// geometry noise than on a clean sculpted crease; the min/max range is
// intentionally normalized to roughly [-1, 1] rather than a physical unit so
// it stays usable without the author needing to know the scale constant.
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
//   h = (cavity_min, cavity_max, reserved, reserved)
// texture_choice: 0 flat/gradient colour, 1 ground, 2 grass, 3 mud, 4 forest,
// 5 a user-imported texture (selected by custom_texture_index into the
// library built by TerrainDisplacementRuntime; -1 = none, treated as choice 0).
// height_min_m/height_max_m of -1e6/1e6, slope_min_deg/slope_max_deg of
// 0/180, and cavity_min/cavity_max of -1/1 are the "wide open, ignore this
// axis" sentinels an authored layer uses when it only cares about the
// others. gradient_strength blends from a flat colour_a (0) to a full
// colour_a->colour_b sweep across the band's own height range (1) -- see the
// "height dependent colour map" comment on asterra_biome_texture_apply.
// enable_flags bit 0/1/2/3 = roughness/metallic/anisotropy/emission override
// active for this layer; a control left off leaves the corresponding output
// exactly as the layers under it (or the terrain's base material) left it,
// rather than forcing every band to also flatten roughness just to recolour
// something. enable_flags bit 4 = height range is relative to the local
// terrain trend rather than absolute sea level (see "Height reference" above).
// h.z is the gradient distribution curve's point count (2-8): the raw
// height fraction that gradient_strength scales is reshaped by this curve
// FIRST (see btex_curve_eval below and CurveFieldData on the CPU side) --
// e.g. an S-curve holds colour_a longer before transitioning, instead of a
// plain linear sweep. The identity curve ([0,0],[1,1]) is a precise no-op.

// BIOME_TEX_LAYER_MAX, u_biome_tex_layer_count/a/b/d/f, and the height/slope/
// noise gating helpers (btex_hash/btex_noise/btex_band) now live in
// terrain_biome_texture_mask.gdshaderinc -- split out so scatter (and any
// other system) can read the SAME authored band data to gate placement
// without pulling in this file's full triplanar material sampling. See that
// file's header for why (vertex-shader-safe, no gpu_surface_pbr dependency).
#include "res://shaders/terrain_biome_texture_mask.gdshaderinc"
uniform vec4 u_biome_tex_layer_c[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_e[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_g[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_h[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_curve_ab[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_curve_cd[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_curve_ef[BIOME_TEX_LAYER_MAX];
uniform vec4 u_biome_tex_layer_curve_gh[BIOME_TEX_LAYER_MAX];

// Screen-space curvature-to-cavity conversion -- see the "Cavity/convexity"
// header comment. Tuned by eye against typical terrain crease/ridge scales,
// not physically derived; if bands feel like they trigger too easily or not
// enough, this is the one number to retune.
const float BTEX_CAVITY_SCALE = 4.0;

// Piecewise-smoothstep response curve -- identical maths to
// terrain_biome_profile.gdshaderinc's bp_curve_eval and
// CurveFieldData.evaluate on the CPU, deliberately duplicated (not shared
// via #include): neither biome shaderinc has an include guard, and both end
// up included together in the same fragment shader (the same reason
// terrain_biome_texture_mask.gdshaderinc's btex_hash duplicates bp_hash
// rather than sharing it).
float btex_curve_eval(vec4 ab, vec4 cd, vec4 ef, vec4 gh, int count, float x) {
	x = clamp(x, 0.0, 1.0);
	int n = clamp(count, 2, 8);
	vec2 pts[8];
	pts[0] = ab.xy;
	pts[1] = ab.zw;
	pts[2] = cd.xy;
	pts[3] = cd.zw;
	pts[4] = ef.xy;
	pts[5] = ef.zw;
	pts[6] = gh.xy;
	pts[7] = gh.zw;
	float result = pts[0].y;
	for (int i = 0; i < 7; i++) {
		if (i > n - 2) {
			break;
		}
		vec2 a = pts[i];
		vec2 b = pts[i + 1];
		result = b.y;
		if (x <= b.x || i == n - 2) {
			float t = clamp((x - a.x) / max(b.x - a.x, 1e-5), 0.0, 1.0);
			result = mix(a.y, b.y, smoothstep(0.0, 1.0, t));
			break;
		}
	}
	return result;
}

// User-imported PBR textures (albedo + optional roughness + optional normal
// map), built once by TerrainDisplacementRuntime from embedded PNG bytes
// into fixed-size array layers (Texture2DArray layers must share one
// resolution) whenever the authored library changes -- not a per-frame cost.
// u_biome_tex_custom_tile_m is the world-space tile size (metres) each slot
// was authored with.
const int BIOME_TEX_CUSTOM_MAX = 8;
uniform int u_biome_tex_custom_count = 0;
uniform sampler2DArray u_biome_tex_custom_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2DArray u_biome_tex_custom_roughness : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2DArray u_biome_tex_custom_normal : hint_normal, filter_linear_mipmap_anisotropic, repeat_enable;
uniform float u_biome_tex_custom_tile_m[BIOME_TEX_CUSTOM_MAX];
// Per-imported-texture "organic tiling" strength (see gpu_surface_antitile
// .gdshaderinc's spat_notile_array header comment) and a direct albedo
// multiply-tint -- both properties of the imported ASSET, not of any one
// band using it, so every band sharing this slot recolours/de-tiles
// consistently. Defaults (0 strength, white tint) exactly reproduce the
// plain single-sample, untinted behaviour every existing authored profile
// already has.
uniform float u_biome_tex_custom_notile[BIOME_TEX_CUSTOM_MAX];
uniform vec3 u_biome_tex_custom_albedo_tint[BIOME_TEX_CUSTOM_MAX];

// btex_hash/btex_noise moved to terrain_biome_texture_mask.gdshaderinc
// (#included above) so scatter's placement gating can reuse the identical
// patchiness noise a band's own material blend uses -- unchanged otherwise.

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

// The four built-ins already had real triplanar normal maps in
// gpu_surface_pbr.gdshaderinc (spbr_tri_normal_*) -- they just were not
// wired into Biome Texture bands before now. A flat colour has nothing to
// perturb the surface with, so it returns the unperturbed `n` unchanged.
vec3 btex_material_normal(int choice, vec3 dpos, vec3 dpdx, vec3 dpdy, vec3 n) {
	if (choice == 1) return spbr_tri_normal_ground(dpos, dpdx, dpdy, n);
	if (choice == 2) return spbr_tri_normal_grass(dpos, dpdx, dpdy, n);
	if (choice == 3) return spbr_tri_normal_mud(dpos, dpdx, dpdy, n);
	if (choice == 4) return spbr_tri_normal_forest(dpos, dpdx, dpdy, n);
	return n;
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
	float notile = u_biome_tex_custom_notile[idx];
	float layer = float(idx);
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	vec3 sampled = spat_notile_array(u_biome_tex_custom_albedo, spbr_uv_x(p, tile_m, s.x),
				spbr_grad_x(dpdx, tile_m, s.x), spbr_grad_x(dpdy, tile_m, s.x), layer, notile).rgb * w.x
		+ spat_notile_array(u_biome_tex_custom_albedo, spbr_uv_y(p, tile_m, s.y),
				spbr_grad_y(dpdx, tile_m, s.y), spbr_grad_y(dpdy, tile_m, s.y), layer, notile).rgb * w.y
		+ spat_notile_array(u_biome_tex_custom_albedo, spbr_uv_z(p, tile_m, s.z),
				spbr_grad_z(dpdx, tile_m, s.z), spbr_grad_z(dpdy, tile_m, s.z), layer, notile).rgb * w.z;
	return sampled * u_biome_tex_custom_albedo_tint[idx];
}

float btex_custom_rough(int idx, vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	if (idx < 0 || idx >= u_biome_tex_custom_count) {
		return 0.9;
	}
	float tile_m = u_biome_tex_custom_tile_m[idx];
	float notile = u_biome_tex_custom_notile[idx];
	float layer = float(idx);
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	return spat_notile_array(u_biome_tex_custom_roughness, spbr_uv_x(p, tile_m, s.x),
				spbr_grad_x(dpdx, tile_m, s.x), spbr_grad_x(dpdy, tile_m, s.x), layer, notile).r * w.x
		+ spat_notile_array(u_biome_tex_custom_roughness, spbr_uv_y(p, tile_m, s.y),
				spbr_grad_y(dpdx, tile_m, s.y), spbr_grad_y(dpdy, tile_m, s.y), layer, notile).r * w.y
		+ spat_notile_array(u_biome_tex_custom_roughness, spbr_uv_z(p, tile_m, s.z),
				spbr_grad_z(dpdx, tile_m, s.z), spbr_grad_z(dpdy, tile_m, s.z), layer, notile).r * w.z;
}

// Same triplanar pattern as spbr_tri_normal_ground/grass/mud/forest in
// gpu_surface_pbr.gdshaderinc, just generic over which sampler2DArray layer
// it reads (spbr_normal_world is that file's tangent-space-to-world
// conversion, reused as-is). A missing normal map defaults its Texture2DArray
// layer to a flat "up" tangent-space fill (see
// TerrainDisplacementRuntime._build_custom_texture_library), so this quietly
// returns the unperturbed geometric normal `n` for a texture that never had
// one imported, exactly like btex_custom_rough defaults to a flat 0.9.
vec3 btex_custom_normal(int idx, vec3 p, vec3 dpdx, vec3 dpdy, vec3 n) {
	if (idx < 0 || idx >= u_biome_tex_custom_count) {
		return n;
	}
	float tile_m = u_biome_tex_custom_tile_m[idx];
	float notile = u_biome_tex_custom_notile[idx];
	float layer = float(idx);
	vec3 w = spbr_weights(n);
	vec3 s = spbr_axis_sign(n);
	vec3 nx = spat_notile_array(u_biome_tex_custom_normal, spbr_uv_x(p, tile_m, s.x),
			spbr_grad_x(dpdx, tile_m, s.x), spbr_grad_x(dpdy, tile_m, s.x), layer, notile).rgb * 2.0 - 1.0;
	vec3 ny = spat_notile_array(u_biome_tex_custom_normal, spbr_uv_y(p, tile_m, s.y),
			spbr_grad_y(dpdx, tile_m, s.y), spbr_grad_y(dpdy, tile_m, s.y), layer, notile).rgb * 2.0 - 1.0;
	vec3 nz = spat_notile_array(u_biome_tex_custom_normal, spbr_uv_z(p, tile_m, s.z),
			spbr_grad_z(dpdx, tile_m, s.z), spbr_grad_z(dpdy, tile_m, s.z), layer, notile).rgb * 2.0 - 1.0;
	return normalize(spbr_normal_world(nx, 0, s.x) * w.x
		+ spbr_normal_world(ny, 1, s.y) * w.y
		+ spbr_normal_world(nz, 2, s.z) * w.z);
}

// A cheap, distance-stable cavity/convexity estimate -- see the header
// comment's "Cavity/convexity" section. `dndx`/`dndy` are the screen-space
// derivatives of the SAME shading normal `dpdx`/`dpdy` were derived
// alongside (computed once by the caller, before this function's own layer
// loop can perturb that normal, for the same reason dpdx/dpdy themselves are
// computed once by the caller rather than re-derived per layer: a value
// computed under a per-fragment-varying branch does not differentiate
// meaningfully). Dividing by the squared position derivative cancels out the
// pixel footprint, so the result approximates true curvature (1/metre)
// rather than something that changes with camera distance or screen
// resolution.
float btex_cavity_value(vec3 dndx, vec3 dndy, vec3 dpdx, vec3 dpdy) {
	float footprint = dot(dpdx, dpdx) + dot(dpdy, dpdy);
	if (footprint < 1e-10) {
		return 0.0;
	}
	float raw = (dot(dndx, dpdx) + dot(dndy, dpdy)) / footprint;
	return clamp(raw * BTEX_CAVITY_SCALE, -1.0, 1.0);
}

// btex_band also moved to terrain_biome_texture_mask.gdshaderinc.

// Composes every layer bound to `biome` on top of the terrain's existing,
// already-classified/textured material, each blended in by its own
// height/slope/cavity/(optional patchiness) coverage weight. `height_m` is
// height above sea level; `local_baseline_m` is the terrain's own broad-scale
// trend at this point, for layers in relative-height mode (see the header
// comment); `slope_deg` is the angle between the shading normal and local
// "up" (0 flat, 90 vertical); `cavity_value` is the caller's precomputed
// btex_cavity_value result. `dpos`/`dpdx`/`dpdy` are the same precision-safe
// detail position/derivatives gpu_surface_pbr.gdshaderinc's own triplanar
// sampling uses, so a texture-choice layer tiles and mipmaps identically to
// the rest of the terrain's material. `n` is the shading normal, mutated
// in place as layers blend in their own material's normal (see the header
// comment) -- the caller must derive `dpdx`/`dpdy`/`cavity_value` from `n`'s
// value BEFORE calling this, not after, since they need the single
// well-defined derivative of the unperturbed normal. `emission_add`
// accumulates additively (see the header comment) rather than blending like
// the others -- the caller adds it into EMISSION once, after this returns.
// Zero-cost when no layer is bound.
void asterra_biome_texture_apply(vec3 dir, int biome, float height_m, float local_baseline_m,
		float slope_deg, float cavity_value,
		vec3 dpos, vec3 dpdx, vec3 dpdy, inout vec3 n,
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
		vec4 f = u_biome_tex_layer_f[i];
		int flags = int(round(f.w));
		float effective_height_m = (flags & 16) != 0 ? (height_m - local_baseline_m) : height_m;
		vec4 h = u_biome_tex_layer_h[i];
		float w = btex_band(effective_height_m, a.z, a.w, b.z)
			* btex_band(slope_deg, b.x, b.y, 2.0)
			* btex_band(cavity_value, h.x, h.y, 0.08);
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
		// height range is the wide-open sentinel), its shape reworked by the
		// gradient distribution curve (h.z point count -- an identity curve
		// leaves this a plain linear lerp, unchanged from before the curve
		// existed). gradient_strength (e.a) then scales how much of that
		// (curved) sweep applies -- 0 keeps a flat colour_a, 1 is the full
		// gradient, in between blends toward it.
		float grad_raw = clamp((effective_height_m - a.z) / max(a.w - a.z, 1e-4), 0.0, 1.0);
		float grad_t = btex_curve_eval(u_biome_tex_layer_curve_ab[i], u_biome_tex_layer_curve_cd[i],
			u_biome_tex_layer_curve_ef[i], u_biome_tex_layer_curve_gh[i], int(round(h.z)), grad_raw);
		vec3 layer_color = mix(c.rgb, e.rgb, grad_t * clamp(e.a, 0.0, 1.0));

		int choice = int(floor(a.y + 0.5));
		int custom_idx = int(round(dd.w));
		float tint = clamp(dd.z, 0.0, 1.0);
		vec3 material_albedo;
		float material_rough;
		vec3 material_normal;
		if (choice == 0) {
			material_albedo = layer_color;
			material_rough = 0.9;
			material_normal = n;
		} else if (choice == 5 && custom_idx >= 0) {
			material_albedo = mix(btex_custom_albedo(custom_idx, dpos, dpdx, dpdy, n), layer_color, tint);
			material_rough = btex_custom_rough(custom_idx, dpos, dpdx, dpdy, n);
			material_normal = btex_custom_normal(custom_idx, dpos, dpdx, dpdy, n);
		} else {
			material_albedo = mix(btex_material_albedo(choice, dpos, dpdx, dpdy, n), layer_color, tint);
			material_rough = btex_material_rough(choice, dpos, dpdx, dpdy, n);
			material_normal = btex_material_normal(choice, dpos, dpdx, dpdy, n);
		}

		float opacity = clamp(c.a, 0.0, 1.0) * w;
		albedo = mix(albedo, material_albedo, opacity);
		// Same "always blends the natural contribution, override or not" as
		// roughness below -- a texture always has *a* normal (even if that's
		// just "unperturbed"), so this needs no enable flag of its own.
		n = normalize(mix(n, material_normal, opacity));

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
