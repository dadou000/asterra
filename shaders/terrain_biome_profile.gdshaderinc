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
//   b = (param [fbm passes 1-4, terrace steps 2-24], seed, 0, 0)
// layer_type: 0 fbm noise, 1 ridged, 2 terraced, 3 water erosion (subtractive),
//             4 sediment deposit (additive). amount_m may be negative to carve.
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

// bp_noise's FMA correction keeps direction * frequency exact (tested clean up
// to this) because that product still lands well under float32's exact-integer
// ceiling (2^24 = 16,777,216 -- above roughly 2^23 even the recovered value
// starts landing on the same quantised integer for neighbouring vertices, and
// no single-correction-term trick fixes that; it would need a true
// double-single pair carried through the whole computation). Clamping here
// means an extreme scale setting degrades to "as fine as float32 supports"
// instead of spiking again above this frequency.
const float BP_MAX_FREQUENCY = 4000000.0;

float bp_fbm(vec3 direction, float scale, int seed, int passes) {
	float total = 0.0;
	float normalizer = 0.0;
	float amplitude = 1.0;
	float frequency = max(abs(scale), 0.000001);
	for (int octave = 0; octave < 4; octave++) {
		if (octave >= clamp(passes, 1, 4)) break;
		total += bp_noise(direction, min(frequency, BP_MAX_FREQUENCY), seed + octave * 1013) * amplitude;
		normalizer += amplitude;
		frequency *= 2.0;
		amplitude *= 0.5;
	}
	return normalizer > 0.0 ? total / normalizer : 0.0;
}

// Metres this one layer contributes at `d`, before the biome boundary weight.
float bp_layer_value(vec3 d, vec4 a, vec4 b) {
	int layer_type = int(floor(a.y + 0.5));
	float scale = a.z;
	float amount = a.w;
	int seed = int(floor(b.y + 0.5));
	int param = int(floor(b.x + 0.5));
	if (layer_type == 1) {
		float ridge = 1.0 - abs(bp_fbm(d, scale, seed, param));
		return ridge * ridge * amount;
	} else if (layer_type == 2) {
		float field = bp_fbm(d, scale, seed, 1);
		float steps = clamp(float(param), 2.0, 24.0);
		float terrace = floor((field * 0.5 + 0.5) * steps) / max(steps - 1.0, 1.0);
		return (terrace * 2.0 - 1.0) * amount;
	} else if (layer_type == 3) {
		float field = bp_fbm(d, scale, seed, 3);
		float channel = clamp(1.0 - abs(field), 0.0, 1.0);
		return -(channel * channel * channel) * amount;
	} else if (layer_type == 4) {
		float field = bp_fbm(d, scale, seed, 3);
		float deposit = clamp(1.0 - abs(field * 1.65), 0.0, 1.0);
		return deposit * deposit * amount;
	}
	return bp_fbm(d, scale, seed, param) * amount;
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

// Metres to add to the surface at `dir`. Zero-cost when no layer is bound.
float asterra_biome_profile_height(vec3 dir) {
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
		total += bp_layer_value(d, a, u_biome_layer_b[i]) * cov;
	}
	return total;
}
