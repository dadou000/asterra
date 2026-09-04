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

float bp_noise(vec3 p, int seed) {
	ivec3 i = ivec3(floor(p));
	vec3 f = fract(p);
	f = f * f * (vec3(3.0) - 2.0 * f);
	float a0 = mix(bp_hash(i, seed), bp_hash(i + ivec3(1, 0, 0), seed), f.x);
	float a1 = mix(bp_hash(i + ivec3(0, 1, 0), seed), bp_hash(i + ivec3(1, 1, 0), seed), f.x);
	float b0 = mix(bp_hash(i + ivec3(0, 0, 1), seed), bp_hash(i + ivec3(1, 0, 1), seed), f.x);
	float b1 = mix(bp_hash(i + ivec3(0, 1, 1), seed), bp_hash(i + ivec3(1, 1, 1), seed), f.x);
	return mix(mix(a0, a1, f.y), mix(b0, b1, f.y), f.z);
}

float bp_fbm(vec3 direction, float scale, int seed, int passes) {
	float total = 0.0;
	float normalizer = 0.0;
	float amplitude = 1.0;
	float frequency = max(abs(scale), 0.000001);
	for (int octave = 0; octave < 4; octave++) {
		if (octave >= clamp(passes, 1, 4)) break;
		total += bp_noise(direction * frequency, seed + octave * 1013) * amplitude;
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

// Deterministic 0..2pi rotation for the sample kernel, matched by the CPU mirror
// so blend-band heights agree for contact.
float bp_kernel_rotation(vec3 d) {
	return fract(sin(d.x * 127.1 + d.y * 311.7 + d.z * 74.7) * 43758.5453) * 6.28318530718;
}

// 0..1 smooth fraction of a rotated tangent-plane disc (centre + two jittered
// rings out to the biome's blend half-width) that resolves to `biome`. The
// per-vertex rotation dithers the discrete biome-id map so the boundary reads as
// a soft gradient rather than the fixed-angle radial banding a static kernel
// produces. With a 0 half-width this is just the hard centre sample.
float bp_biome_coverage(vec3 d, vec3 tx, vec3 ty, int biome) {
	float centre = (int(floor(ASTERRA_BIOME_ID_AT(d) + 0.5)) == biome) ? 1.0 : 0.0;
	float half_m = u_biome_blend_m[clamp(biome, 0, 17)];
	if (half_m <= 1.0) {
		return centre;
	}
	float ang = half_m / max(u_planet_radius, 1.0);
	float rot = bp_kernel_rotation(d);
	float acc = centre * 1.5;
	float wsum = 1.5;
	for (int ring = 0; ring < 2; ring++) {
		float radius = mix(0.55, 1.0, float(ring));
		float ring_w = mix(1.0, 0.6, float(ring));
		for (int k = 0; k < 6; k++) {
			float theta = rot + float(k) * 1.04719755 + float(ring) * 0.5235988;
			vec3 sd = normalize(d + (tx * cos(theta) + ty * sin(theta)) * (ang * radius));
			acc += ring_w * ((int(floor(ASTERRA_BIOME_ID_AT(sd) + 0.5)) == biome) ? 1.0 : 0.0);
			wsum += ring_w;
		}
	}
	return smoothstep(0.10, 0.90, acc / wsum);
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

// Metres to add to the surface at `dir`, plus a tangent-plane tilt (downslope
// vector) so the vertex shader can correct the shading normal for the biome
// displacement without a faceted screen-space derivative. `biome_id_hint` is
// accepted for call-site compatibility; the boundary kernel resolves the biome
// itself. Zero-cost when no layer is bound.
float asterra_biome_profile_height_grad(vec3 dir, out vec3 tilt) {
	tilt = vec3(0.0);
	if (u_biome_layer_count <= 0) {
		return 0.0;
	}
	vec3 d = normalize(dir);
	vec3 tx;
	vec3 ty;
	bp_tangent_basis(d, tx, ty);
	float eps_m = 2.0;
	float eps_ang = eps_m / max(u_planet_radius, 1.0);
	vec3 dx = normalize(d + tx * eps_ang);
	vec3 dy = normalize(d + ty * eps_ang);
	float h0 = 0.0;
	float hx = 0.0;
	float hy = 0.0;
	int cached_biome = -1;
	float cov = 0.0;
	for (int i = 0; i < BIOME_LAYER_MAX; i++) {
		if (i >= u_biome_layer_count) {
			break;
		}
		vec4 a = u_biome_layer_a[i];
		vec4 b = u_biome_layer_b[i];
		int layer_biome = int(floor(a.x + 0.5));
		if (layer_biome != cached_biome) {
			cached_biome = layer_biome;
			cov = bp_biome_coverage(d, tx, ty, layer_biome);
		}
		if (cov <= 0.0) {
			continue;
		}
		h0 += bp_layer_value(d, a, b) * cov;
		hx += bp_layer_value(dx, a, b) * cov;
		hy += bp_layer_value(dy, a, b) * cov;
	}
	tilt = -tx * ((hx - h0) / eps_m) - ty * ((hy - h0) / eps_m);
	return h0;
}

float asterra_biome_profile_height(vec3 dir, float biome_id_hint) {
	vec3 unused_tilt;
	return asterra_biome_profile_height_grad(dir, unused_tilt);
}
