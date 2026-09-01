uniform sampler2D u_author_disp_code : filter_nearest, repeat_disable;
uniform int u_author_disp_count = 0;
uniform int u_author_disp_output = -1;
uniform float u_author_disp_ready = 0.0;
// Mirrored by TerrainDisplacementRuntime; this is per-vertex local storage.
const int AUTHOR_DISP_MAX_INSTRUCTIONS = 32;

float ad_hash(ivec3 p, int seed) {
	uint h = uint(p.x) * 374761393u + uint(p.y) * 668265263u + uint(p.z) * 2147483647u + uint(seed) * 1274126177u;
	h = (h ^ (h >> 13u)) * 1274126177u;
	h ^= h >> 16u;
	return float(h & 0x00ffffffu) / 16777215.0 * 2.0 - 1.0;
}

float ad_noise(vec3 p, int seed) {
	ivec3 i = ivec3(floor(p));
	vec3 f = fract(p);
	f = f * f * (vec3(3.0) - 2.0 * f);
	float a0 = mix(ad_hash(i, seed), ad_hash(i + ivec3(1,0,0), seed), f.x);
	float a1 = mix(ad_hash(i + ivec3(0,1,0), seed), ad_hash(i + ivec3(1,1,0), seed), f.x);
	float b0 = mix(ad_hash(i + ivec3(0,0,1), seed), ad_hash(i + ivec3(1,0,1), seed), f.x);
	float b1 = mix(ad_hash(i + ivec3(0,1,1), seed), ad_hash(i + ivec3(1,1,1), seed), f.x);
	return mix(mix(a0, a1, f.y), mix(b0, b1, f.y), f.z);
}

float ad_terrain_fbm(vec3 direction, float scale, int seed, int passes) {
	float total = 0.0;
	float normalizer = 0.0;
	float amplitude = 1.0;
	float frequency = max(abs(scale), 0.000001);
	for (int octave = 0; octave < 4; octave++) {
		if (octave >= clamp(passes, 1, 4)) break;
		float sample_value = ad_noise(direction * frequency, seed + octave * 1013);
		total += sample_value * amplitude;
		normalizer += amplitude;
		frequency *= 2.0;
		amplitude *= 0.5;
	}
	return normalizer > 0.0 ? total / normalizer : 0.0;
}

float ad_reg(float values[AUTHOR_DISP_MAX_INSTRUCTIONS], int index, int current) {
	if (index < 0 || index >= current || index >= AUTHOR_DISP_MAX_INSTRUCTIONS) return 0.0;
	return values[index];
}

float author_displacement_height(vec3 direction, float base_height_m, float biome_id, int clipmap_level, float time_s) {
	if (u_author_disp_ready < 0.5 || u_author_disp_count <= 0 || u_author_disp_output < 0) return 0.0;
	float values[AUTHOR_DISP_MAX_INSTRUCTIONS];
	for (int i = 0; i < AUTHOR_DISP_MAX_INSTRUCTIONS; i++) values[i] = 0.0;
	int count = min(u_author_disp_count, AUTHOR_DISP_MAX_INSTRUCTIONS);
	for (int i = 0; i < AUTHOR_DISP_MAX_INSTRUCTIONS; i++) {
		if (i >= count) break;
		vec4 h = texelFetch(u_author_disp_code, ivec2(0, i), 0);
		vec4 p = texelFetch(u_author_disp_code, ivec2(1, i), 0);
		int op = int(round(h.x));
		float a = ad_reg(values, int(round(h.y)), i);
		float b = ad_reg(values, int(round(h.z)), i);
		float c = ad_reg(values, int(round(h.w)), i);
		float v = 0.0;
		if (op == 0) v = p.x;
		else if (op == 1) v = base_height_m;
		else if (op == 2) v = biome_id;
		else if (op == 3) v = float(clipmap_level);
		else if (op == 4) v = time_s;
		else if (op == 5) v = ad_noise(normalize(direction) * p.x, int(round(p.y)));
		else if (op == 6) v = a + b;
		else if (op == 7) v = a - b;
		else if (op == 8) v = a * b;
		else if (op == 9) v = abs(b) > 1e-12 ? a / b : 0.0;
		else if (op == 10) v = min(a, b);
		else if (op == 11) v = max(a, b);
		else if (op == 12) v = abs(a);
		else if (op == 13) v = abs(a) > 1e-12 ? sign(a) * pow(abs(a), b) : 0.0;
		else if (op == 14) v = clamp(a, min(b, c), max(b, c));
		else if (op == 15) v = smoothstep(b, c, a);
		else if (op == 16) { float t = abs(c-b) > 1e-12 ? (a-b)/(c-b) : 0.0; v = mix(p.x, p.y, t); }
		else if (op == 17) v = mix(a, b, clamp(c, 0.0, 1.0));
		else if (op == 18) v = a * p.x;
		else if (op == 19) { int mask = int(round(p.x)); int level = clamp(clipmap_level, 0, 30); v = (mask & (1 << level)) != 0 ? a : 0.0; }
		else if (op == 20) { int mode = int(round(p.x)); int mask = int(round(p.y)); int bid = clamp(int(round(biome_id)), 0, 17); bool selected = (mask & (1 << bid)) != 0; bool allow = mode == 1 ? selected : (mode == 2 ? selected == false : true); v = allow ? a : 0.0; }
		else if (op == 21) v = b;
		else if (op == 22) v = a + ad_terrain_fbm(normalize(direction), p.x, int(round(p.w)), int(round(p.z))) * p.y;
		else if (op == 23) { float ridge = 1.0 - abs(ad_terrain_fbm(normalize(direction), p.x, int(round(p.w)), int(round(p.z)))); v = a + ridge * ridge * p.y; }
		else if (op == 24) { float field = ad_terrain_fbm(normalize(direction), p.x, int(round(p.w)), int(round(p.z))); float channel = clamp(1.0 - abs(field), 0.0, 1.0); v = a - channel * channel * channel * p.y; }
		else if (op == 25) { float field = ad_terrain_fbm(normalize(direction), p.x, int(round(p.w)), int(round(p.z))); float deposit = clamp(1.0 - abs(field * 1.65), 0.0, 1.0); v = a + deposit * deposit * p.y; }
		values[i] = v;
	}
	return values[clamp(u_author_disp_output, 0, count - 1)];
}
