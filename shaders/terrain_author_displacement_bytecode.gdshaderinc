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

float ad_wrap_degrees(float degrees) {
	return mod(degrees + 180.0, 360.0) - 180.0;
}

float ad_positive_arc_degrees(float from_deg, float to_deg) {
	return mod(to_deg - from_deg + 360.0, 360.0);
}

float ad_latitude_mask(vec3 direction, vec4 p) {
	vec3 d = normalize(direction);
	float latitude_deg = asin(clamp(d.y, -1.0, 1.0)) * 57.29577951308232;
	float south_deg = min(p.x, p.y);
	float north_deg = max(p.x, p.y);
	float feather_deg = max(p.z, 0.0);
	float lower = latitude_deg >= south_deg ? 1.0 : 0.0;
	float upper = latitude_deg <= north_deg ? 1.0 : 0.0;
	if (feather_deg > 1e-6) {
		lower = smoothstep(south_deg - feather_deg, south_deg, latitude_deg);
		upper = 1.0 - smoothstep(north_deg, north_deg + feather_deg, latitude_deg);
	}
	float value = clamp(lower * upper, 0.0, 1.0);
	return mod(round(p.w), 2.0) >= 0.5 ? 1.0 - value : value;
}

float ad_longitude_mask(vec3 direction, vec4 p) {
	vec3 d = normalize(direction);
	// 0 degrees is +Z, +90 degrees is +X. Work in a positive eastward arc so
	// west > east naturally represents a band crossing the +/-180 degree seam.
	float longitude_deg = atan(d.x, d.z) * 57.29577951308232;
	float west_deg = ad_wrap_degrees(p.x);
	float east_deg = ad_wrap_degrees(p.y);
	float feather_deg = max(p.z, 0.0);
	float span_deg = ad_positive_arc_degrees(west_deg, east_deg);
	float along_deg = ad_positive_arc_degrees(west_deg, longitude_deg);
	float value = 0.0;
	if (span_deg <= 1e-6) {
		// Equal edges intentionally mean a full 360 degree band, not an empty mask.
		value = 1.0;
	} else if (along_deg <= span_deg) {
		// Match Latitude Band: the requested interior remains fully selected and
		// feather only softens the exterior of either circular edge.
		value = 1.0;
	} else if (feather_deg > 1e-6) {
		float after_east = along_deg - span_deg;
		float before_west = 360.0 - along_deg;
		float edge_distance = min(after_east, before_west);
		value = 1.0 - smoothstep(0.0, feather_deg, edge_distance);
	}
	value = clamp(value, 0.0, 1.0);
	return mod(round(p.w), 2.0) >= 0.5 ? 1.0 - value : value;
}

vec3 ad_direction_from_latlon(float latitude_deg, float longitude_deg) {
	float latitude_rad = latitude_deg * 0.017453292519943295;
	float longitude_rad = longitude_deg * 0.017453292519943295;
	float cos_latitude = cos(latitude_rad);
	return vec3(
		sin(longitude_rad) * cos_latitude,
		sin(latitude_rad),
		cos(longitude_rad) * cos_latitude);
}

float ad_radial_mask(vec3 direction, vec4 p) {
	vec3 d = normalize(direction);
	vec3 center_direction = ad_direction_from_latlon(p.x, p.y);
	float angle_deg = acos(clamp(dot(d, center_direction), -1.0, 1.0)) * 57.29577951308232;
	float radius_deg = max(p.z, 0.0);
	float feather_deg = abs(p.w);
	if (p.w < 0.0) {
		// Private Ring Area inner-edge mode: select at/outside the radius and put
		// the feather only inside the excluded center hole.
		if (angle_deg >= radius_deg) return 1.0;
		if (feather_deg <= 1e-6) return 0.0;
		return clamp(smoothstep(radius_deg - feather_deg, radius_deg, angle_deg), 0.0, 1.0);
	}
	if (radius_deg >= 180.0 - 1e-6 || angle_deg <= radius_deg) return 1.0;
	if (feather_deg <= 1e-6) return 0.0;
	return clamp(1.0 - smoothstep(radius_deg, radius_deg + feather_deg, angle_deg), 0.0, 1.0);
}

float ad_reg(float values[AUTHOR_DISP_MAX_INSTRUCTIONS], int index, int current) {
	if (index < 0 || index >= current || index >= AUTHOR_DISP_MAX_INSTRUCTIONS) return 0.0;
	return values[index];
}

float author_displacement_height_with_sculpt(vec3 direction, float base_height_m,
		float biome_id, int clipmap_level, float time_s, float sculpt_delta_m) {
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
		else if (op == 26) v = sculpt_delta_m;
		else if (op == 27) v = round(p.w) >= 2.0 ? ad_longitude_mask(direction, p) : ad_latitude_mask(direction, p);
		else if (op == 28) v = ad_radial_mask(direction, p);
		values[i] = v;
	}
	return values[clamp(u_author_disp_output, 0, count - 1)];
}

// Backward-compatible entry point for Blank terrain and older shader callers.
float author_displacement_height(vec3 direction, float base_height_m, float biome_id,
		int clipmap_level, float time_s) {
	return author_displacement_height_with_sculpt(direction, base_height_m, biome_id,
		clipmap_level, time_s, 0.0);
}
