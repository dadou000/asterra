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
// Floating-origin offset (Frames.origin wrapped modulo BP_ORIGIN_WRAP_M) so the
// high-frequency layer noise can be sampled from a bounded world position and
// stay locked to the world without float32 aliasing. Pushed per frame by the
// renderer; 0 = sample straight off the unit direction (legacy).
uniform vec3 u_biome_detail_origin = vec3(0.0);

const float BP_ORIGIN_WRAP_M = 4096.0;
// Lattice wrap for the value-noise hash. Keeps floor()/fract() inputs small so
// they stay precise at very high frequency; the noise tiles every BP_LATTICE
// cells, which for the frequencies this path is used at is far outside any view.
const float BP_LATTICE = 65536.0;
const int BP_LATTICE_I = 65536;

float bp_hash(ivec3 p, int seed) {
	p = ((p % BP_LATTICE_I) + BP_LATTICE_I) % BP_LATTICE_I;
	uint h = uint(p.x) * 374761393u + uint(p.y) * 668265263u + uint(p.z) * 2147483647u + uint(seed) * 1274126177u;
	h = (h ^ (h >> 13u)) * 1274126177u;
	h ^= h >> 16u;
	return float(h & 0x00ffffffu) / 16777215.0 * 2.0 - 1.0;
}

float bp_noise(vec3 p, int seed) {
	p -= floor(p / BP_LATTICE) * BP_LATTICE;
	ivec3 i = ivec3(floor(p));
	vec3 f = fract(p);
	f = f * f * (vec3(3.0) - 2.0 * f);
	float a0 = mix(bp_hash(i, seed), bp_hash(i + ivec3(1, 0, 0), seed), f.x);
	float a1 = mix(bp_hash(i + ivec3(0, 1, 0), seed), bp_hash(i + ivec3(1, 1, 0), seed), f.x);
	float b0 = mix(bp_hash(i + ivec3(0, 0, 1), seed), bp_hash(i + ivec3(1, 0, 1), seed), f.x);
	float b1 = mix(bp_hash(i + ivec3(0, 1, 1), seed), bp_hash(i + ivec3(1, 1, 1), seed), f.x);
	return mix(mix(a0, a1, f.y), mix(b0, b1, f.y), f.z);
}

// `direction` is the unit surface direction; `sample_p` is a bounded world
// position (render-space + wrapped detail origin). For coarse octaves the unit
// direction is precise enough and never seams, so it is used directly. For fine
// octaves the frequency is snapped so an origin rebase (a multiple of
// BP_ORIGIN_WRAP_M) shifts the noise coordinate by a whole number of lattice
// cells -- seamless once bp_noise wraps -- and the bounded sample_p keeps full
// float precision.
float bp_fbm(vec3 direction, vec3 sample_p, float scale, int seed, int passes) {
	float total = 0.0;
	float normalizer = 0.0;
	float amplitude = 1.0;
	float frequency = max(abs(scale), 0.000001);
	float radius = max(u_planet_radius, 1.0);
	for (int octave = 0; octave < 4; octave++) {
		if (octave >= clamp(passes, 1, 4)) break;
		vec3 q;
		if (frequency > 256.0) {
			float snap = max(1.0, floor(frequency * BP_ORIGIN_WRAP_M / radius + 0.5))
				* radius / BP_ORIGIN_WRAP_M;
			q = (sample_p / radius) * snap;
		} else {
			q = direction * frequency;
		}
		total += bp_noise(q, seed + octave * 1013) * amplitude;
		normalizer += amplitude;
		frequency *= 2.0;
		amplitude *= 0.5;
	}
	return normalizer > 0.0 ? total / normalizer : 0.0;
}

// Metres this one layer contributes at `d`, before the biome boundary weight.
float bp_layer_value(vec3 d, vec3 sample_p, vec4 a, vec4 b) {
	int layer_type = int(floor(a.y + 0.5));
	float scale = a.z;
	float amount = a.w;
	int seed = int(floor(b.y + 0.5));
	int param = int(floor(b.x + 0.5));
	if (layer_type == 1) {
		float ridge = 1.0 - abs(bp_fbm(d, sample_p, scale, seed, param));
		return ridge * ridge * amount;
	} else if (layer_type == 2) {
		float field = bp_fbm(d, sample_p, scale, seed, 1);
		float steps = clamp(float(param), 2.0, 24.0);
		float terrace = floor((field * 0.5 + 0.5) * steps) / max(steps - 1.0, 1.0);
		return (terrace * 2.0 - 1.0) * amount;
	} else if (layer_type == 3) {
		float field = bp_fbm(d, sample_p, scale, seed, 3);
		float channel = clamp(1.0 - abs(field), 0.0, 1.0);
		return -(channel * channel * channel) * amount;
	} else if (layer_type == 4) {
		float field = bp_fbm(d, sample_p, scale, seed, 3);
		float deposit = clamp(1.0 - abs(field * 1.65), 0.0, 1.0);
		return deposit * deposit * amount;
	}
	return bp_fbm(d, sample_p, scale, seed, param) * amount;
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

// Metres to add to the surface at `dir`. `world_sample_p` is a bounded,
// world-locked position (render-space position + the wrapped floating-origin
// offset) that high-frequency layers sample from instead of `dir * frequency`,
// so very small features stay precise instead of aliasing. It only affects
// noise phase, not the resulting height's correctness, so an approximate
// (pre-displacement) position is fine. The boundary kernel resolves the biome
// itself. Zero-cost when no layer is bound.
float asterra_biome_profile_height(vec3 dir, vec3 world_sample_p) {
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
		total += bp_layer_value(d, world_sample_p, a, u_biome_layer_b[i]) * cov;
	}
	return total;
}
