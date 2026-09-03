// Per-biome terrain profile: base shape (+ optional water erosion and sediment)
// masked to one biome, composed on top of the shared Global Terrain.
//
// This deliberately does NOT use the authored displacement bytecode VM. That VM
// keeps a 32-slot local register array that has triggered Vulkan device loss on
// current NVIDIA drivers when a big surface shader recompiles around it. Biome
// profiles are a fixed, small shape, so they lower to a handful of uniforms and a
// bounded loop with no local arrays -- always safe to compile, and free (one
// branch) whenever u_biome_profile_count is 0.
//
// Packing per profile (mirrored by BiomeProfileRuntime on the CPU):
//   a = (target_biome_id, base_type[0 fbm / 1 ridged / 2 terraced], base_scale, base_amount_m)
//   b = (base_param[passes 1-4 or terrace steps 2-24], seed, erosion_amount_m, erosion_scale)
//   c = (sediment_amount_m, sediment_scale, strength, unused)

const int BIOME_PROFILE_MAX = 6;
uniform int u_biome_profile_count = 0;
uniform vec4 u_biome_profile_a[BIOME_PROFILE_MAX];
uniform vec4 u_biome_profile_b[BIOME_PROFILE_MAX];
uniform vec4 u_biome_profile_c[BIOME_PROFILE_MAX];

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

// Metres to add to the surface at `dir` for the biome under it. Zero-cost when no
// profile is bound.
float asterra_biome_profile_height(vec3 dir, float biome_id) {
	if (u_biome_profile_count <= 0) {
		return 0.0;
	}
	vec3 d = normalize(dir);
	int here = int(floor(biome_id + 0.5));
	float total = 0.0;
	for (int i = 0; i < BIOME_PROFILE_MAX; i++) {
		if (i >= u_biome_profile_count) {
			break;
		}
		vec4 a = u_biome_profile_a[i];
		if (int(floor(a.x + 0.5)) != here) {
			continue;
		}
		vec4 b = u_biome_profile_b[i];
		vec4 c = u_biome_profile_c[i];
		int base_type = int(floor(a.y + 0.5));
		int seed = int(floor(b.y + 0.5));
		float base;
		if (base_type == 1) {
			float ridge = 1.0 - abs(bp_fbm(d, a.z, seed, int(floor(b.x + 0.5))));
			base = ridge * ridge * a.w;
		} else if (base_type == 2) {
			float field = bp_fbm(d, a.z, seed, 1);
			float steps = clamp(floor(b.x + 0.5), 2.0, 24.0);
			float terrace = floor((field * 0.5 + 0.5) * steps) / max(steps - 1.0, 1.0);
			base = (terrace * 2.0 - 1.0) * a.w;
		} else {
			base = bp_fbm(d, a.z, seed, int(floor(b.x + 0.5))) * a.w;
		}
		float h = base;
		if (b.z > 0.0) {
			float field = bp_fbm(d, b.w, seed + 7919, 3);
			float channel = clamp(1.0 - abs(field), 0.0, 1.0);
			h -= channel * channel * channel * b.z;
		}
		if (c.x > 0.0) {
			float field = bp_fbm(d, c.y, seed + 104729, 3);
			float deposit = clamp(1.0 - abs(field * 1.65), 0.0, 1.0);
			h += deposit * deposit * c.x;
		}
		total += h * c.z;
	}
	return total;
}
