// Per-biome terrain: a stack of displacement layers masked to one biome, with a
// kilometre-scale soft boundary so neighbouring biomes' terrain cross-fades
// instead of meeting at a hard texel edge. Composed on top of the shared Global
// Terrain.
//
// This deliberately does NOT use the authored displacement bytecode VM. That VM
// keeps a 32-slot local register array that has triggered Vulkan device loss on
// current NVIDIA drivers when a big surface shader recompiles around it. Biome
// layers are a fixed, small shape set, so they lower to a handful of uniforms and
// bounded loops with no local arrays -- always safe to compile, and free (one
// branch) whenever u_biome_layer_count is 0.
//
// Packing per layer (mirrored by TerrainDisplacementRuntime on the CPU):
//   a = (biome_id, layer_type, scale, amount_m)
//   b = (param [fbm passes 1-4, terrace steps 2-24], seed,
//        angle_deg [layer_type 3/4 only -- flow/ridge orientation], 0)
// layer_type: 0 fbm noise, 1 ridged, 2 terraced, 3 water erosion (subtractive,
//             domain-warped ridged channels -- see bp_erosion_channels), 4
//             sediment deposit (additive, slope-biased talus -- see
//             bp_sediment_deposit). amount_m may be negative to carve.
// Layers are packed grouped by biome_id so the boundary kernel is evaluated once
// per biome, not once per layer.
//
// u_biome_blend_m[b] is the transition half-width in metres for biome b (0 = hard
// edge, the old behaviour). ASTERRA_BIOME_ID_AT(dir) is supplied by the including
// shader (procedural context texture, or the Blank authored-biome override).

#ifndef ASTERRA_BIOME_ID_AT
#define ASTERRA_BIOME_ID_AT(p) (0.0)
#endif
// Bilinearly-filtered 0..1 biome membership from the including shader; already a
// smooth ramp about one context texel wide. Falls back to a hard test on the
// categorical id when the including shader does not provide it.
#ifndef ASTERRA_BIOME_MEMBERSHIP
#define ASTERRA_BIOME_MEMBERSHIP(p, b) ((int(floor(ASTERRA_BIOME_ID_AT(p) + 0.5)) == (b)) ? 1.0 : 0.0)
#endif

const int BIOME_LAYER_MAX = 16;
uniform int u_biome_layer_count = 0;
uniform vec4 u_biome_layer_a[BIOME_LAYER_MAX];
uniform vec4 u_biome_layer_b[BIOME_LAYER_MAX];
uniform float u_biome_blend_m[18];

float bp_hash(ivec3 p, int seed) {
	uint h = uint(p.x) * 374761393u + uint(p.y) * 668265263u + uint(p.z) * 2147483647u + uint(seed) * 1274126177u;
	h = (h ^ (h >> 13u)) * 1274126177u;
	h ^= h >> 16u;
	return float(h & 0x00ffffffu) / 16777215.0 * 2.0 - 1.0;
}

// Value noise sampled at `direction * frequency`, computed at float32-pair
// ("double-single") precision rather than a single lossy float32 multiply.
// `direction * frequency` alone rounds to ~24 total mantissa bits, so above
// roughly 256x the fractional part is quantised to a handful of steps and
// neighbouring vertices alias into a visibly stepped/spiky surface -- and any
// fix that instead samples from a bounded coordinate relative to some moving
// reference frame (Frames.origin's floating-origin rebase, the clipmap's own
// stable-anchor re-anchor) just relocates the discontinuity to fire whenever
// that frame resets, which is still a visible pop, just a rarer one.
// `fma(a, b, -a*b)` recovers the *exact* rounding error of one float32
// multiply (Dekker's TwoProduct via FMA, exact on IEEE754 hardware), so
// `fract(p) + e` restores the low bits the single multiply rounded away.
// This needs no reference frame at all -- `direction` alone is already
// precise near the observer (see spherical_geometry_clipmap.gd's
// anchor-relative direction reconstruction), so this is exact at any
// frequency across every rebase and every re-anchor, not just the common one.
float bp_noise(vec3 direction, float frequency, int seed) {
	vec3 p = direction * frequency;
	vec3 e = vec3(
		fma(direction.x, frequency, -p.x),
		fma(direction.y, frequency, -p.y),
		fma(direction.z, frequency, -p.z));
	// e is the low bits the p = direction * frequency rounding threw away --
	// at high frequency it is NOT negligible (it can be a sizeable fraction
	// of one whole lattice cell), so fract(p) + e routinely lands outside
	// [0, 1). A plain fract() would wrap that back in without telling the
	// integer lattice index below, so the vertex would hash against the
	// WRONG cell -- uncorrelated with its neighbours, i.e. an isolated spike.
	// floor() on the raw sum gives the correct carry into the cell index
	// instead, so every vertex's hash lookup stays consistent with its
	// neighbours'.
	vec3 pf = floor(p);
	vec3 raw = (p - pf) + e;
	vec3 carry = floor(raw);
	vec3 f = raw - carry;
	ivec3 i = ivec3(pf + carry);
	f = f * f * (vec3(3.0) - 2.0 * f);
	float a0 = mix(bp_hash(i, seed), bp_hash(i + ivec3(1, 0, 0), seed), f.x);
	float a1 = mix(bp_hash(i + ivec3(0, 1, 0), seed), bp_hash(i + ivec3(1, 1, 0), seed), f.x);
	float b0 = mix(bp_hash(i + ivec3(0, 0, 1), seed), bp_hash(i + ivec3(1, 0, 1), seed), f.x);
	float b1 = mix(bp_hash(i + ivec3(0, 1, 1), seed), bp_hash(i + ivec3(1, 1, 1), seed), f.x);
	return mix(mix(a0, a1, f.y), mix(b0, b1, f.y), f.z);
}

// bp_noise's FMA correction keeps direction * frequency exact well past this
// (tested clean to 4,000,000 for a single bp_noise call, since that product
// still lands well under float32's exact-integer ceiling of 2^24 = 16,777,216
// -- above roughly 2^23 even the recovered value starts landing on the same
// quantised integer for neighbouring vertices, and no single-correction-term
// trick fixes that; it would need a true double-single pair carried through
// the whole computation). bp_erosion_channels/bp_sediment_deposit compound
// several bp_noise calls and small-angle offsets on top, which eats into that
// margin -- tested clean only to ~1,500,000 -- so this shared ceiling (one
// constant, used by every layer type) is set there rather than at bp_noise's
// own higher limit. Clamping here means an extreme scale setting degrades to
// "as fine as float32 supports" instead of spiking again above this frequency.
const float BP_MAX_FREQUENCY = 1500000.0;

// Fades out an octave whose spatial wavelength the current mesh cannot
// represent without aliasing. One full noise cycle of `direction * frequency`
// spans roughly `u_planet_radius / frequency` metres of world arc length; once
// that wavelength approaches the local vertex spacing, neighbouring vertices
// sample effectively uncorrelated parts of the noise field no matter how
// precise the math is (bp_noise's FMA correction fixes float32 precision, not
// the mesh's own triangle density) -- this is exactly the "individual
// voxels"/crystalline aliasing an extreme Feature Size setting produces,
// since scale is no longer capped to what any mesh could resolve now that
// BP_MAX_FREQUENCY allows it up to float32's own limit. Ramping to zero over
// [1, 3] vertices-per-wavelength (rather than a hard cutoff at 1) avoids a
// visible seam where the fade kicks in.
float bp_octave_fade(float frequency, float spacing_m) {
	if (spacing_m <= 0.0) {
		return 1.0;
	}
	float wavelength_m = u_planet_radius / max(frequency, 0.000001);
	return smoothstep(spacing_m, spacing_m * 3.0, wavelength_m);
}

// `spacing_m` is the current vertex-to-vertex spacing (0 to disable the
// fade), used to apply bp_octave_fade to every octave's own amplitude AND to
// the normalizer -- so a faded-out fine octave does not just dim the whole
// result, the remaining (resolvable) octaves are renormalised to their own
// full relative weight, the way fbm naturally looks at any zoom level.
float bp_fbm(vec3 direction, float scale, int seed, int passes, float spacing_m) {
	float total = 0.0;
	float normalizer = 0.0;
	float amplitude = 1.0;
	float frequency = max(abs(scale), 0.000001);
	for (int octave = 0; octave < 4; octave++) {
		if (octave >= clamp(passes, 1, 4)) break;
		float f = min(frequency, BP_MAX_FREQUENCY);
		float w = bp_octave_fade(f, spacing_m) * amplitude;
		total += bp_noise(direction, f, seed + octave * 1013) * w;
		normalizer += w;
		frequency *= 2.0;
		amplitude *= 0.5;
	}
	return normalizer > 0.0 ? total / normalizer : 0.0;
}

// Branchless orthonormal tangent basis for a unit vector (Duff et al. 2017),
// smooth everywhere except n.z == -1 -- so the kernel has no seam.
void bp_tangent_basis(vec3 n, out vec3 tx, out vec3 ty) {
	float s = n.z >= 0.0 ? 1.0 : -1.0;
	float a = -1.0 / (s + n.z);
	float b = n.x * n.y * a;
	tx = vec3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
	ty = vec3(b, s + n.y * n.y * a, -n.y);
}

// Rotates an orthonormal tangent-plane basis by an authored angle. The raw
// bp_tangent_basis frame (Duff et al.) is picked for numerical stability, not
// for any particular "north" -- combined with the noise lattice's own
// (unavoidably slightly anisotropic, since the hash weights x/y/z
// differently) structure, that leaves erosion channels / sediment flow with a
// fixed default orientation. Rotating the basis before it is used for flow
// direction rotates the resulting ridge/valley orientation with it, and
// costs nothing extra when angle_deg is 0.
void bp_rotate_basis(vec3 tx, vec3 ty, float angle_deg, out vec3 rtx, out vec3 rty) {
	float a = radians(angle_deg);
	float ca = cos(a);
	float sa = sin(a);
	rtx = tx * ca + ty * sa;
	rty = ty * ca - tx * sa;
}

// Fake hydraulic erosion: warps the noise-sample direction by lower-frequency
// noise before evaluating a ridged pattern, so channels bend and branch the
// way water-carved valleys do instead of the isotropic blobs a plain ridged
// noise gives (a real flow-accumulation simulation needs the whole height
// field at once -- global state a per-vertex shader doesn't have -- so this is
// the standard real-time stand-in for it, not a simulation). A single,
// low-frequency "flow" warp is shared by every octave so the overall channel
// direction stays coherent across scales (one connected valley, not
// decorrelated per-octave noise); each octave then adds its own smaller warp
// on top for branching detail at that octave's scale, echoing how a real
// drainage network's tributaries are self-similar at every scale.
// `angle_deg` rotates the flow/channel orientation (see bp_rotate_basis).
// `spacing_m` fades unresolvable octaves the same way bp_fbm does (see
// bp_octave_fade) -- 0 disables the fade.
float bp_erosion_channels(vec3 d, float scale, int seed, int passes, float angle_deg,
		float spacing_m) {
	vec3 tx0;
	vec3 ty0;
	bp_tangent_basis(d, tx0, ty0);
	vec3 tx;
	vec3 ty;
	bp_rotate_basis(tx0, ty0, angle_deg, tx, ty);
	// Clamped up front, not just where each derived frequency is used: an
	// unclamped base_frequency pushed far enough past BP_MAX_FREQUENCY can
	// still put flow_frequency (0.12x base_frequency) AT the ceiling instead
	// of comfortably under it, converging with an octave's own (separately
	// clamped) frequency and losing the two-scale separation this relies on
	// to stay smooth. Clamping base_frequency here keeps flow_frequency a
	// fixed, safe fraction of the ceiling no matter how extreme the input.
	float base_frequency = min(max(abs(scale), 0.000001), BP_MAX_FREQUENCY);
	float flow_frequency = base_frequency * 0.12;
	float flow_x = bp_noise(d, flow_frequency, seed + 5101);
	float flow_y = bp_noise(d, flow_frequency, seed + 7247);
	float total = 0.0;
	float normalizer = 0.0;
	float amplitude = 1.0;
	float frequency = base_frequency;
	for (int octave = 0; octave < 4; octave++) {
		if (octave >= clamp(passes, 1, 4)) break;
		float f = min(frequency, BP_MAX_FREQUENCY);
		float branch_x = bp_noise(d, f, seed + octave * 733 + 11);
		float branch_y = bp_noise(d, f, seed + octave * 733 + 19);
		// Floored the same way bp_sediment_deposit floors its gradient step:
		// 1 / f alone shrinks below float32 epsilon near BP_MAX_FREQUENCY, at
		// which point `d + tiny_offset` can round straight back to `d` and the
		// warp (and the branching it produces) silently vanishes.
		float warp_scale = max(1.0 / max(f, 1.0), 3.0e-6);
		vec3 warped = normalize(d + (tx * (flow_x * 1.4 + branch_x * 0.5)
			+ ty * (flow_y * 1.4 + branch_y * 0.5)) * warp_scale);
		float ridge = 1.0 - abs(bp_noise(warped, f, seed + octave * 1013));
		float channel = ridge * ridge * ridge;
		float w = bp_octave_fade(f, spacing_m) * amplitude;
		total += channel * w;
		normalizer += w;
		frequency *= 2.0;
		amplitude *= 0.5;
	}
	return normalizer > 0.0 ? clamp(total / normalizer, 0.0, 1.0) : 0.0;
}

// Landslide-style talus deposits: shifted toward a locally coherent "downhill"
// bias direction instead of a symmetric blob, so sediment reads as debris that
// slid and piled up, and weighted by a steepness estimate so it only appears
// where the ground actually reads as rough, tapering to nothing on flat
// ground the way real talus does.
//
// An earlier version derived the bias direction from a finite-difference
// GRADIENT of the ridged field (three extra bp_noise taps, then subtract and
// divide by the tap spacing) -- a genuine steepest-descent direction, but its
// DIRECTION turned out to rotate sharply within a single lattice cell near any
// saddle point of the underlying noise field (there are many, in any rough
// noise), and feeding that into another noise sample's position spiked hard
// even though the gradient itself is mathematically continuous. This instead
// borrows bp_erosion_channels' "flow" trick for the direction: two
// INDEPENDENT low-frequency noise fields read directly and used as a 2D
// vector. No finite difference, no cancellation, no saddle-point rotation --
// just two smooth fields, so it moves smoothly by construction. The tradeoff
// is that this bias direction is no longer literally the steepest-descent
// direction of anything, just a spatially coherent "which way debris tends to
// have slid around here" look -- true hydraulic/talus physics needs the whole
// height field's flow at once (global state a per-vertex shader doesn't have)
// no matter which of these two techniques picks the direction. `angle_deg`
// rotates the bias direction (see bp_rotate_basis). `spacing_m` fades
// unresolvable octaves the same way bp_fbm does (see bp_octave_fade) -- 0
// disables the fade.
float bp_sediment_deposit(vec3 d, float scale, int seed, int passes, float angle_deg,
		float spacing_m) {
	vec3 tx0;
	vec3 ty0;
	bp_tangent_basis(d, tx0, ty0);
	vec3 tx;
	vec3 ty;
	bp_rotate_basis(tx0, ty0, angle_deg, tx, ty);
	float base_frequency = min(max(abs(scale), 0.000001), BP_MAX_FREQUENCY);
	// Both the steepness estimate and the bias direction work off a much
	// lower frequency than the deposit texture itself -- a landslide responds
	// to the broad shape of a slope, not to noise-level bumps -- which also
	// keeps everything below from jittering at the deposit's own fine noise
	// frequency (mirrors bp_erosion_channels' shared "flow" warp).
	float slope_frequency = min(max(base_frequency * 0.15, 0.000001), BP_MAX_FREQUENCY);
	// Floored well above float32 epsilon (~1.19e-7 near d's own magnitude): at
	// BP_MAX_FREQUENCY, 0.6 / frequency alone would be ~1.5e-7, small enough
	// that `d + tx * step_ang` can round straight back to `d` -- the steepness
	// estimate then silently reads as zero instead of just getting less
	// precise. On the CPU (double precision, no vanishing risk) the same
	// floor is still used so both sides estimate the same steepness.
	float step_ang = max(0.6 / max(slope_frequency, 1.0), 3.0e-6);
	float h0 = 1.0 - abs(bp_noise(d, slope_frequency, seed + 9001));
	vec3 dx = normalize(d + tx * step_ang);
	vec3 dy = normalize(d + ty * step_ang);
	float hx = 1.0 - abs(bp_noise(dx, slope_frequency, seed + 9001));
	float hy = 1.0 - abs(bp_noise(dy, slope_frequency, seed + 9001));
	// Used only as a scalar blend weight below (never as a direction), so its
	// own small-scale wobble at most flickers intensity, not sample position --
	// unlike the discarded gradient-direction approach above, this is safe.
	// Bounded to roughly [-1, 1] regardless of frequency since h0/hx/hy each
	// live in [0, 1]; NOT normalised by step_ang, whose scale drifts with
	// frequency and would saturate a fixed threshold at every usable one.
	float steepness = length(vec2(hx - h0, hy - h0));
	float slope_w = smoothstep(0.02, 0.35, steepness);

	float flow_x = bp_noise(d, slope_frequency, seed + 5101);
	float flow_y = bp_noise(d, slope_frequency, seed + 7247);
	float shift_scale = max(1.6 / max(slope_frequency, 1.0), 8.0e-6);
	// One shared downhill-bias shift for every octave -- so the whole deposit
	// pattern slides coherently in one direction as a unit, the way a real
	// talus fan does, instead of each octave shifting on its own separate,
	// fine-noise-frequency terms.
	vec2 shift = vec2(flow_x, flow_y) * shift_scale;

	float total = 0.0;
	float normalizer = 0.0;
	float amplitude = 1.0;
	float frequency = base_frequency;
	for (int octave = 0; octave < 4; octave++) {
		if (octave >= clamp(passes, 1, 4)) break;
		float f = min(frequency, BP_MAX_FREQUENCY);
		vec3 shifted = normalize(d - (tx * shift.x + ty * shift.y));
		float field = bp_noise(shifted, f, seed + octave * 1013);
		float deposit = clamp(1.0 - abs(field * 1.65), 0.0, 1.0);
		float w = bp_octave_fade(f, spacing_m) * amplitude;
		total += deposit * deposit * w;
		normalizer += w;
		frequency *= 2.0;
		amplitude *= 0.5;
	}
	return normalizer > 0.0 ? clamp(total / normalizer, 0.0, 1.0) * slope_w : 0.0;
}

// Metres this one layer contributes at `d`, before the biome boundary weight.
// `spacing_m` is the current vertex spacing, for bp_octave_fade.
float bp_layer_value(vec3 d, vec4 a, vec4 b, float spacing_m) {
	int layer_type = int(floor(a.y + 0.5));
	float scale = a.z;
	float amount = a.w;
	int seed = int(floor(b.y + 0.5));
	int param = int(floor(b.x + 0.5));
	if (layer_type == 1) {
		float ridge = 1.0 - abs(bp_fbm(d, scale, seed, param, spacing_m));
		return ridge * ridge * amount;
	} else if (layer_type == 2) {
		float field = bp_fbm(d, scale, seed, 1, spacing_m);
		float steps = clamp(float(param), 2.0, 24.0);
		float terrace = floor((field * 0.5 + 0.5) * steps) / max(steps - 1.0, 1.0);
		return (terrace * 2.0 - 1.0) * amount;
	} else if (layer_type == 3) {
		float channel = bp_erosion_channels(d, scale, seed, param, b.z, spacing_m);
		return -channel * amount;
	} else if (layer_type == 4) {
		float deposit = bp_sediment_deposit(d, scale, seed, param, b.z, spacing_m);
		return deposit * amount;
	}
	return bp_fbm(d, scale, seed, param, spacing_m) * amount;
}

// 0..1 boundary coverage of `biome`.
//
// The earlier passes sampled the HARD categorical biome id on a kernel and tried
// to smooth the result -- a fixed grid terraces, a jittered grid spikes, a
// high-frequency dither loses float precision and spikes. The fix is to sample a
// signal that is already continuous: ASTERRA_BIOME_MEMBERSHIP is a bilinearly
// filtered membership, a C0 ramp ~one context texel wide. A small ring of
// continuous-offset taps then widens that ramp toward the blend half-width;
// every input is smooth and moves continuously with `d`, so nothing terraces.
float bp_biome_coverage(vec3 d, vec3 tx, vec3 ty, int biome) {
	float half_m = u_biome_blend_m[clamp(biome, 0, 17)];
	if (half_m <= 1.0) {
		return (int(floor(ASTERRA_BIOME_ID_AT(d) + 0.5)) == biome) ? 1.0 : 0.0;
	}
	float ang = half_m / max(u_planet_radius, 1.0);
	float acc = ASTERRA_BIOME_MEMBERSHIP(d, biome) * 1.6;
	float wsum = 1.6;
	for (int k = 0; k < 6; k++) {
		float theta = float(k) * 1.04719755;
		vec3 inner = normalize(d + (tx * cos(theta) + ty * sin(theta)) * (ang * 0.5));
		vec3 outer = normalize(d + (tx * cos(theta) + ty * sin(theta)) * ang);
		acc += 0.8 * ASTERRA_BIOME_MEMBERSHIP(inner, biome);
		acc += 0.45 * ASTERRA_BIOME_MEMBERSHIP(outer, biome);
		wsum += 1.25;
	}
	return smoothstep(0.18, 0.82, acc / wsum);
}

// Metres to add to the surface at `dir`. `spacing_m` is the current
// vertex-to-vertex spacing in metres (pass 0 to disable the geometric
// anti-aliasing fade -- see bp_octave_fade); every layer type fades its own
// unresolvable octaves toward zero rather than aliasing into a crystalline,
// "individual voxels visible" look once its Feature Size setting produces a
// wavelength finer than the mesh can represent. Zero-cost when no layer is
// bound.
float asterra_biome_profile_height(vec3 dir, float spacing_m) {
	if (u_biome_layer_count <= 0) {
		return 0.0;
	}
	vec3 d = normalize(dir);
	vec3 tx;
	vec3 ty;
	bp_tangent_basis(d, tx, ty);
	float total = 0.0;
	int cached_biome = -1;
	float cov = 0.0;
	for (int i = 0; i < BIOME_LAYER_MAX; i++) {
		if (i >= u_biome_layer_count) {
			break;
		}
		vec4 a = u_biome_layer_a[i];
		int layer_biome = int(floor(a.x + 0.5));
		if (layer_biome != cached_biome) {
			cached_biome = layer_biome;
			cov = bp_biome_coverage(d, tx, ty, layer_biome);
		}
		if (cov <= 0.0) {
			continue;
		}
		total += bp_layer_value(d, a, u_biome_layer_b[i], spacing_m) * cov;
	}
	return total;
}
