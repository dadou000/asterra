uniform sampler2D u_author_mat_code : filter_nearest, repeat_disable;
uniform int u_author_mat_count = 0;
uniform int u_author_mat_output_albedo = -1;
uniform int u_author_mat_output_normal = -1;
uniform int u_author_mat_output_roughness = -1;
uniform int u_author_mat_output_metallic = -1;
uniform int u_author_mat_output_ao = -1;
uniform int u_author_mat_output_specular = -1;
uniform float u_author_mat_ready = 0.0;
uniform int u_author_mat_texture_ready_mask = 0;
uniform sampler2D u_author_mat_tex0 : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_author_mat_tex1 : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_author_mat_tex2 : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_author_mat_tex3 : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_author_mat_tex4 : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_author_mat_tex5 : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_author_mat_tex6 : source_color, filter_linear_mipmap, repeat_enable;
uniform sampler2D u_author_mat_tex7 : source_color, filter_linear_mipmap, repeat_enable;

// Mirrored by TerrainMaterialRuntime; this is per-fragment local storage. Keep the
// register file at 32 even though opcode IDs now extend above 32.
const int AUTHOR_MAT_MAX_INSTRUCTIONS = 32;

float am_hash(ivec3 p, int seed) {
	uint h = uint(p.x) * 374761393u + uint(p.y) * 668265263u
		+ uint(p.z) * 2147483647u + uint(seed) * 1274126177u;
	h = (h ^ (h >> 13u)) * 1274126177u;
	h ^= h >> 16u;
	return float(h & 0x00ffffffu) / 16777215.0 * 2.0 - 1.0;
}

float am_noise(vec3 p, int seed) {
	ivec3 i = ivec3(floor(p));
	vec3 f = fract(p);
	f = f * f * (vec3(3.0) - 2.0 * f);
	float a0 = mix(am_hash(i, seed), am_hash(i + ivec3(1,0,0), seed), f.x);
	float a1 = mix(am_hash(i + ivec3(0,1,0), seed), am_hash(i + ivec3(1,1,0), seed), f.x);
	float b0 = mix(am_hash(i + ivec3(0,0,1), seed), am_hash(i + ivec3(1,0,1), seed), f.x);
	float b1 = mix(am_hash(i + ivec3(0,1,1), seed), am_hash(i + ivec3(1,1,1), seed), f.x);
	return mix(mix(a0, a1, f.y), mix(b0, b1, f.y), f.z);
}

vec4 am_reg(vec4 values[AUTHOR_MAT_MAX_INSTRUCTIONS], int index, int current) {
	if (index < 0 || index >= current || index >= AUTHOR_MAT_MAX_INSTRUCTIONS) return vec4(0.0);
	return values[index];
}

bool am_texture_ready(int index) {
	if (index < 0 || index > 7) return false;
	return (u_author_mat_texture_ready_mask & (1 << index)) != 0;
}

vec4 am_sample_texture(int index, vec2 uv) {
	if (!am_texture_ready(index)) return vec4(1.0);
	if (index == 0) return texture(u_author_mat_tex0, uv);
	if (index == 1) return texture(u_author_mat_tex1, uv);
	if (index == 2) return texture(u_author_mat_tex2, uv);
	if (index == 3) return texture(u_author_mat_tex3, uv);
	if (index == 4) return texture(u_author_mat_tex4, uv);
	if (index == 5) return texture(u_author_mat_tex5, uv);
	if (index == 6) return texture(u_author_mat_tex6, uv);
	return texture(u_author_mat_tex7, uv);
}

vec2 am_equirect_uv(vec3 direction) {
	vec3 d = normalize(direction);
	float u = atan(d.z, d.x) / 6.28318530718 + 0.5;
	float v = asin(clamp(d.y, -1.0, 1.0)) / 3.14159265359 + 0.5;
	return vec2(u, 1.0 - v);
}

vec4 am_triplanar(int texture_index, vec3 position, vec3 normal, float scale) {
	vec3 n = abs(normalize(normal));
	n = pow(max(n, vec3(0.0001)), vec3(4.0));
	n /= max(n.x + n.y + n.z, 0.0001);
	vec3 p = position * max(abs(scale), 0.000001);
	vec4 x = am_sample_texture(texture_index, fract(p.zy));
	vec4 y = am_sample_texture(texture_index, fract(p.xz));
	vec4 z = am_sample_texture(texture_index, fract(p.xy));
	return x * n.x + y * n.y + z * n.z;
}

void author_material_apply(
		vec3 direction,
		vec3 world_position,
		float terrain_height_m,
		float slope,
		float biome_id,
		int clipmap_level,
		float camera_distance_m,
		float time_s,
		vec4 soil,
		vec4 surface,
		vec4 geology,
		vec4 structure,
		vec4 climate,
		vec4 hydrology,
		vec4 landform,
		vec4 material_primary,
		vec4 material_secondary,
		float rock_id,
		float micro_layer,
		float rock_mix,
		inout vec3 albedo,
		inout vec3 surface_normal,
		inout float roughness,
		inout float specular,
		inout float metallic,
		inout float ao) {
	if (u_author_mat_ready < 0.5 || u_author_mat_count <= 0) return;

	vec4 values[AUTHOR_MAT_MAX_INSTRUCTIONS];
	for (int i = 0; i < AUTHOR_MAT_MAX_INSTRUCTIONS; i++) values[i] = vec4(0.0);
	int count = min(u_author_mat_count, AUTHOR_MAT_MAX_INSTRUCTIONS);
	vec4 base_albedo = vec4(albedo, 1.0);
	vec4 base_normal = vec4(normalize(surface_normal), 1.0);
	vec4 base_roughness = vec4(roughness);
	vec4 base_metallic = vec4(metallic);
	vec4 base_ao = vec4(ao);
	vec4 base_specular = vec4(specular);

	for (int i = 0; i < AUTHOR_MAT_MAX_INSTRUCTIONS; i++) {
		if (i >= count) break;
		vec4 h = texelFetch(u_author_mat_code, ivec2(0, i), 0);
		vec4 p = texelFetch(u_author_mat_code, ivec2(1, i), 0);
		int op = int(round(h.x));
		vec4 a = am_reg(values, int(round(h.y)), i);
		vec4 b = am_reg(values, int(round(h.z)), i);
		vec4 c = am_reg(values, int(round(h.w)), i);
		vec4 v = vec4(0.0);

		if (op == 0) v = p;
		else if (op == 1) v = base_albedo;
		else if (op == 2) v = base_normal;
		else if (op == 3) v = base_roughness;
		else if (op == 4) v = base_metallic;
		else if (op == 5) v = base_ao;
		else if (op == 6) v = vec4(terrain_height_m);
		else if (op == 7) v = vec4(biome_id);
		else if (op == 8) v = vec4(float(clipmap_level));
		else if (op == 9) v = vec4(time_s);
		else if (op == 10) v = vec4(slope);
		else if (op == 11) v = vec4(normalize(direction), 1.0);
		else if (op == 12) v = vec4(world_position, 1.0);
		else if (op == 13) v = vec4(normalize(surface_normal), 1.0);
		else if (op == 14) v = vec4(camera_distance_m);
		else if (op == 15) v = vec4(am_noise(normalize(direction) * p.x, int(round(p.y))));
		else if (op == 16) v = a + b;
		else if (op == 17) v = a - b;
		else if (op == 18) v = a * b;
		else if (op == 19) v = vec4(
			abs(b.x) > 1e-12 ? a.x / b.x : 0.0,
			abs(b.y) > 1e-12 ? a.y / b.y : 0.0,
			abs(b.z) > 1e-12 ? a.z / b.z : 0.0,
			abs(b.w) > 1e-12 ? a.w / b.w : 0.0);
		else if (op == 20) v = min(a, b);
		else if (op == 21) v = max(a, b);
		else if (op == 22) v = abs(a);
		else if (op == 23) v = sign(a) * pow(max(abs(a), vec4(1e-12)), b);
		else if (op == 24) v = clamp(a, min(b, c), max(b, c));
		else if (op == 25) v = smoothstep(b, c, a);
		else if (op == 26) {
			vec4 denom = c - b;
			vec4 t = vec4(
				abs(denom.x) > 1e-12 ? (a.x-b.x)/denom.x : 0.0,
				abs(denom.y) > 1e-12 ? (a.y-b.y)/denom.y : 0.0,
				abs(denom.z) > 1e-12 ? (a.z-b.z)/denom.z : 0.0,
				abs(denom.w) > 1e-12 ? (a.w-b.w)/denom.w : 0.0);
			v = mix(vec4(p.x), vec4(p.y), t);
		}
		else if (op == 27) v = mix(a, b, clamp(c, vec4(0.0), vec4(1.0)));
		else if (op == 28) {
			vec2 uv = fract(am_equirect_uv(direction) * max(abs(p.y), 0.000001));
			v = am_sample_texture(int(round(p.x)), uv);
		}
		else if (op == 29) v = am_triplanar(int(round(p.x)), world_position,
			surface_normal, max(abs(a.x), 0.000001));
		else if (op == 30) {
			vec3 n = normalize(a.xyz + b.xyz);
			v = vec4(n, 1.0);
		}
		else if (op == 31) {
			int level_mask = int(round(p.x));
			int level = clamp(clipmap_level, 0, 30);
			bool level_allowed = (level_mask & (1 << level)) != 0;
			int mode = int(round(p.y));
			int biome_mask = int(round(p.z));
			int bid = clamp(int(round(biome_id)), 0, 17);
			bool selected = (biome_mask & (1 << bid)) != 0;
			bool biome_allowed = mode == 1 ? selected : (mode == 2 ? !selected : true);
			v = vec4(level_allowed && biome_allowed ? 1.0 : 0.0);
		}
		else if (op == 32) {
			int blend_mode = int(round(p.x));
			float strength = p.y;
			float mask = clamp(c.x, 0.0, 1.0);
			vec4 candidate = b * strength;
			vec4 composed = a + candidate;
			if (blend_mode == 1) composed = a - candidate;
			else if (blend_mode == 2) composed = a * candidate;
			else if (blend_mode == 3) composed = min(a, candidate);
			else if (blend_mode == 4) composed = max(a, candidate);
			else if (blend_mode == 5) composed = candidate;
			v = mix(a, composed, mask);
		}
		else if (op == 33) v = base_specular;
		else if (op == 34) v = vec4(mix(-40.0, 50.0, climate.r));
		else if (op == 35) v = vec4(climate.g * 3000.0);
		else if (op == 36) v = vec4(climate.a * 55.0);
		else if (op == 37) v = vec4(surface.g);
		else if (op == 38) v = vec4(surface.b);
		else if (op == 39) v = vec4(soil.r);
		else if (op == 40) v = vec4(soil.g);
		else if (op == 41) v = vec4(soil.b);
		else if (op == 42) v = vec4(surface.r * 14.0);
		else if (op == 43) v = vec4(surface.a * 160.0);
		else if (op == 44) v = vec4(rock_id);
		else if (op == 45) v = vec4(geology.r * 1.85);
		else if (op == 46) v = vec4(geology.g * 1.2);
		else if (op == 47) v = vec4(structure.r * 2.0 - 1.0);
		else if (op == 48) v = vec4(hydrology.r * 2.0 - 1.0);
		else if (op == 49) v = vec4(hydrology.g * 2.0 - 1.0);
		else if (op == 50) v = hydrology;
		else if (op == 51) v = material_primary;
		else if (op == 52) v = material_secondary;
		else if (op == 53) v = vec4(micro_layer);
		else if (op == 54) v = soil;
		else if (op == 55) v = surface;
		else if (op == 56) v = geology;
		else if (op == 57) v = structure;
		else if (op == 58) v = climate;
		else if (op == 59) v = landform;
		else if (op == 60) v = vec4(rock_mix);
		values[i] = v;
	}

	if (u_author_mat_output_albedo >= 0 && u_author_mat_output_albedo < count)
		albedo = max(values[u_author_mat_output_albedo].rgb, vec3(0.0));
	if (u_author_mat_output_normal >= 0 && u_author_mat_output_normal < count) {
		vec3 n = values[u_author_mat_output_normal].xyz;
		if (dot(n, n) > 1e-10) surface_normal = normalize(n);
	}
	if (u_author_mat_output_roughness >= 0 && u_author_mat_output_roughness < count)
		roughness = clamp(values[u_author_mat_output_roughness].x, 0.0, 1.0);
	if (u_author_mat_output_metallic >= 0 && u_author_mat_output_metallic < count)
		metallic = clamp(values[u_author_mat_output_metallic].x, 0.0, 1.0);
	if (u_author_mat_output_ao >= 0 && u_author_mat_output_ao < count)
		ao = clamp(values[u_author_mat_output_ao].x, 0.0, 1.0);
	if (u_author_mat_output_specular >= 0 && u_author_mat_output_specular < count)
		specular = clamp(values[u_author_mat_output_specular].x, 0.0, 1.0);
}
