// GPU terrain-height evaluator used by systems that must agree with the active
// procedural terrain renderer. Keep this in lock-step with
// spherical_geometry_clipmap_procedural_uv.gdshader.
//
// Requires surface_face_uv() from planet_surface_lookup.gdshaderinc and a
// u_planet_radius uniform supplied by the including shader.

uniform sampler2DArray u_macro_elevation : filter_linear_mipmap, repeat_disable;
uniform float u_macro_ready = 0.0;
uniform float u_macro_face_res = 0.0;
uniform float u_base_spacing = 0.75;
uniform int u_detail_seed = 1337;
uniform float u_detail_strength = 1.0;

const float GPU_TERRAIN_MACRO_FILTER_FOOTPRINT_VERTICES = 3.0;
const float GPU_TERRAIN_MACRO_MAX_LOD = 5.0;
const int GPU_TERRAIN_MACRO_MAX_MIP = 5;
const int GPU_TERRAIN_MAX_LEVEL = 14;

float gpu_terrain_smootherstep01(float x) {
	x = clamp(x, 0.0, 1.0);
	return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

float gpu_terrain_smootherstep_range(float edge0, float edge1, float x) {
	return gpu_terrain_smootherstep01((x - edge0) / max(edge1 - edge0, 1e-9));
}

float gpu_terrain_ring_spacing(int level) {
	return u_base_spacing * exp2(float(level));
}

vec3 gpu_terrain_macro_array_coord(vec3 dir) {
	vec3 fuv = surface_face_uv(dir);
	ivec3 size3 = textureSize(u_macro_elevation, 0);
	float texture_res = float(size3.x);
	float gutter = max((texture_res - u_macro_face_res) * 0.5, 0.0);
	vec2 uv = (fuv.xy * u_macro_face_res + vec2(gutter)) / texture_res;
	return vec3(uv, fuv.z);
}

float gpu_terrain_macro_lod_for_spacing(float spacing_m) {
	float macro_texel_m = (PI * 0.5 * u_planet_radius) / max(u_macro_face_res, 1.0);
	float footprint_m = max(spacing_m * GPU_TERRAIN_MACRO_FILTER_FOOTPRINT_VERTICES, macro_texel_m);
	return clamp(log2(footprint_m / macro_texel_m), 0.0, GPU_TERRAIN_MACRO_MAX_LOD);
}

vec4 gpu_terrain_cubic_bspline_weights(float t) {
	float t2 = t * t;
	float t3 = t2 * t;
	return vec4(
		(1.0 - 3.0 * t + 3.0 * t2 - t3) / 6.0,
		(4.0 - 6.0 * t2 + 3.0 * t3) / 6.0,
		(1.0 + 3.0 * t + 3.0 * t2 - 3.0 * t3) / 6.0,
		t3 / 6.0);
}

float gpu_terrain_macro_bicubic_mip(vec3 coord, int mip) {
	ivec3 size3 = textureSize(u_macro_elevation, mip);
	vec2 size2 = vec2(size3.xy);
	vec2 p = coord.xy * size2 - vec2(0.5);
	vec2 base = floor(p);
	vec2 f = fract(p);

	vec4 wx = gpu_terrain_cubic_bspline_weights(f.x);
	vec4 wy = gpu_terrain_cubic_bspline_weights(f.y);
	float gx0 = wx.x + wx.y;
	float gx1 = wx.z + wx.w;
	float gy0 = wy.x + wy.y;
	float gy1 = wy.z + wy.w;

	float x0 = base.x - 1.0 + wx.y / max(gx0, 1e-9);
	float x1 = base.x + 1.0 + wx.w / max(gx1, 1e-9);
	float y0 = base.y - 1.0 + wy.y / max(gy0, 1e-9);
	float y1 = base.y + 1.0 + wy.w / max(gy1, 1e-9);

	vec2 uv00 = (vec2(x0, y0) + vec2(0.5)) / size2;
	vec2 uv10 = (vec2(x1, y0) + vec2(0.5)) / size2;
	vec2 uv01 = (vec2(x0, y1) + vec2(0.5)) / size2;
	vec2 uv11 = (vec2(x1, y1) + vec2(0.5)) / size2;
	float fmip = float(mip);

	float s00 = textureLod(u_macro_elevation, vec3(uv00, coord.z), fmip).r;
	float s10 = textureLod(u_macro_elevation, vec3(uv10, coord.z), fmip).r;
	float s01 = textureLod(u_macro_elevation, vec3(uv01, coord.z), fmip).r;
	float s11 = textureLod(u_macro_elevation, vec3(uv11, coord.z), fmip).r;

	float row0 = mix(s00, s10, gx1);
	float row1 = mix(s01, s11, gx1);
	return mix(row0, row1, gy1);
}

float gpu_terrain_macro_cubic_trilinear(vec3 coord, float lod) {
	int mip0 = clamp(int(floor(lod)), 0, GPU_TERRAIN_MACRO_MAX_MIP);
	int mip1 = min(mip0 + 1, GPU_TERRAIN_MACRO_MAX_MIP);
	float t = fract(lod);
	float h0 = gpu_terrain_macro_bicubic_mip(coord, mip0);
	if (mip0 == mip1 || t <= 0.001) {
		return h0;
	}
	float h1 = gpu_terrain_macro_bicubic_mip(coord, mip1);
	return mix(h0, h1, gpu_terrain_smootherstep01(t));
}

float gpu_terrain_macro_height(vec3 d, int level) {
	if (u_macro_ready < 0.5 || u_macro_face_res <= 0.5) {
		return 0.0;
	}
	float lod = gpu_terrain_macro_lod_for_spacing(gpu_terrain_ring_spacing(level));
	return gpu_terrain_macro_cubic_trilinear(gpu_terrain_macro_array_coord(d), lod);
}

uint gpu_terrain_hash(ivec3 p, uint seed) {
	uint h = uint(p.x) * 0x8da6b343u;
	h ^= uint(p.y) * 0xd8163841u;
	h ^= uint(p.z) * 0xcb1ab31fu;
	h ^= seed * 0x9e3779b9u;
	h ^= h >> uint(16);
	h *= 0x7feb352du;
	h ^= h >> uint(15);
	h *= 0x846ca68bu;
	h ^= h >> uint(16);
	return h;
}

float gpu_terrain_lattice_value(ivec3 p, uint seed) {
	uint h = gpu_terrain_hash(p, seed) & 0x00ffffffu;
	return float(h) * (2.0 / 16777215.0) - 1.0;
}

float gpu_terrain_smooth_value_noise(vec3 p, uint seed) {
	ivec3 i = ivec3(floor(p));
	vec3 f = fract(p);
	f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	float n000 = gpu_terrain_lattice_value(i + ivec3(0, 0, 0), seed);
	float n100 = gpu_terrain_lattice_value(i + ivec3(1, 0, 0), seed);
	float n010 = gpu_terrain_lattice_value(i + ivec3(0, 1, 0), seed);
	float n110 = gpu_terrain_lattice_value(i + ivec3(1, 1, 0), seed);
	float n001 = gpu_terrain_lattice_value(i + ivec3(0, 0, 1), seed);
	float n101 = gpu_terrain_lattice_value(i + ivec3(1, 0, 1), seed);
	float n011 = gpu_terrain_lattice_value(i + ivec3(0, 1, 1), seed);
	float n111 = gpu_terrain_lattice_value(i + ivec3(1, 1, 1), seed);
	float nx00 = mix(n000, n100, f.x);
	float nx10 = mix(n010, n110, f.x);
	float nx01 = mix(n001, n101, f.x);
	float nx11 = mix(n011, n111, f.x);
	return mix(mix(nx00, nx10, f.y), mix(nx01, nx11, f.y), f.z);
}

float gpu_terrain_band_weight(float wavelength_m, float spacing_m) {
	return gpu_terrain_smootherstep_range(spacing_m * 4.0, spacing_m * 8.0, wavelength_m);
}

float gpu_terrain_detail_band(vec3 world_m, float wavelength_m, float spacing_m,
		uint seed, float amplitude_m) {
	float w = gpu_terrain_band_weight(wavelength_m, spacing_m);
	if (w <= 0.001) {
		return 0.0;
	}
	return gpu_terrain_smooth_value_noise(world_m / wavelength_m, seed) * amplitude_m * w;
}

float gpu_terrain_procedural_detail(vec3 d, int level, float macro_h) {
	float spacing = gpu_terrain_ring_spacing(level);
	vec3 world_m = d * u_planet_radius;
	uint seed = uint(max(u_detail_seed, 1));
	float coast_guard = mix(0.38, 1.0,
		gpu_terrain_smootherstep_range(45.0, 320.0, abs(macro_h)));
	float mountain = gpu_terrain_smootherstep_range(500.0, 2600.0, max(macro_h, 0.0));
	float regional_gain = mix(0.72, 1.30, mountain);
	float h = 0.0;
	h += gpu_terrain_detail_band(world_m, 12000.0, spacing, seed + 11u,
		mix(52.0, 115.0, mountain));
	float w2400 = gpu_terrain_band_weight(2400.0, spacing);
	if (w2400 > 0.001) {
		float n = gpu_terrain_smooth_value_noise(world_m / 2400.0, seed + 23u);
		float ridge = (1.0 - abs(n)) * 2.0 - 1.0;
		h += (n * 28.0 + ridge * 18.0 * mountain) * w2400;
	}
	h += gpu_terrain_detail_band(world_m, 480.0, spacing, seed + 37u,
		mix(7.0, 15.0, mountain));
	h += gpu_terrain_detail_band(world_m, 96.0, spacing, seed + 53u, 3.4);
	h += gpu_terrain_detail_band(world_m, 24.0, spacing, seed + 71u, 0.85);
	return h * coast_guard * regional_gain * u_detail_strength;
}

float gpu_terrain_height_for_level(vec3 d, int level) {
	int safe_level = clamp(level, 0, GPU_TERRAIN_MAX_LEVEL);
	float macro_h = gpu_terrain_macro_height(d, safe_level);
	return macro_h + gpu_terrain_procedural_detail(d, safe_level, macro_h);
}

float gpu_terrain_height_with_morph(vec3 d, int level, float morph) {
	int safe_level = clamp(level, 0, GPU_TERRAIN_MAX_LEVEL);
	float fine = gpu_terrain_height_for_level(d, safe_level);
	if (safe_level < GPU_TERRAIN_MAX_LEVEL && morph > 0.001) {
		float coarse = gpu_terrain_height_for_level(d, safe_level + 1);
		fine = mix(fine, coarse, morph);
	}
	return fine;
}
