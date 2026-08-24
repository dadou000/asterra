shader_type spatial;
render_mode cull_disabled, diffuse_burley, specular_schlick_ggx;

#include "res://shaders/material_clipmap.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/planet_lighting.gdshaderinc"
#include "res://shaders/cloud_shadow.gdshaderinc"

// Robust procedural terrain shader. Logical clipmap coordinates come from UV,
// not VERTEX_ID, so compact sector meshes cannot scramble the grid. The macro
// field is reconstructed with a cubic B-spline in the vertex shader instead of
// hardware bilinear interpolation; this preserves smooth curvature between the
// low-frequency samples instead of turning every macro texel into a planar patch.
uniform sampler2DArray u_macro_elevation : filter_linear_mipmap, repeat_disable;
uniform float u_macro_ready = 0.0;
uniform float u_macro_face_res = 0.0;

uniform float u_planet_radius = 1000000.0;
uniform vec3 u_origin = vec3(0.0);
uniform vec3 u_center_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_center_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_center_up = vec3(0.0, 1.0, 0.0);
uniform float u_base_spacing = 0.75;
uniform float u_grid_cells = 400.0;
uniform float u_visible_cap_angle = 1.5707963;
uniform float u_surface_bias = 0.035;
uniform float u_height_enabled = 1.0;
uniform float u_sink_scale = 2.0;
uniform int u_detail_seed = 1337;
uniform float u_detail_strength = 1.0;
uniform float u_debug_side_cut = 0.0;

varying vec3 v_planet;
varying vec3 v_render;
varying vec3 v_smooth_normal;
varying float v_valid;
varying float v_cap_valid;
varying float v_level;
varying float v_direct_transmittance;

const int ASTERRA_MAX_LEVEL = 14;
const float RING_INNER_Q = 0.44;
const float RING_HANDOFF_Q = 0.50;
const float FINE_MORPH_START_Q = 0.875;
const float MAX_PROJECTED_THETA = 1.62;
const float TANGENT_APPROX_MAX_ARC_M = 90000.0;
const float MACRO_FILTER_FOOTPRINT_VERTICES = 3.0;
const float MACRO_MAX_LOD = 5.0;
const int MACRO_MAX_MIP = 5;

// Normals deliberately use a broader footprint than the geometry. The height
// field stays fully detailed, but lighting no longer follows individual clipmap
// triangle planes as the lattice snaps underneath a moving camera.
const float NORMAL_SAMPLE_SPACING_MULT = 3.0;
const float NORMAL_SAMPLE_MIN_M = 4.0;

float ring_spacing(int level) {
	return u_base_spacing * exp2(float(level));
}

float smootherstep01(float x) {
	x = clamp(x, 0.0, 1.0);
	return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

float smootherstep_range(float edge0, float edge1, float x) {
	return smootherstep01((x - edge0) / max(edge1 - edge0, 1e-9));
}

vec3 direction_for_offset(vec2 offset_m) {
	float arc = length(offset_m);
	if (arc < 1e-5) {
		return normalize(u_center_dir);
	}
	if (arc <= TANGENT_APPROX_MAX_ARC_M) {
		return normalize(u_center_dir
			+ u_center_right * (offset_m.x / u_planet_radius)
			+ u_center_up * (offset_m.y / u_planet_radius));
	}
	float raw_theta = arc / max(u_planet_radius, 1.0);
	float theta = min(raw_theta, min(u_visible_cap_angle * 1.02, MAX_PROJECTED_THETA));
	vec3 tangent = normalize(u_center_right * offset_m.x + u_center_up * offset_m.y);
	return normalize(u_center_dir * cos(theta) + tangent * sin(theta));
}

vec3 macro_array_coord(vec3 dir) {
	vec3 fuv = surface_face_uv(dir);
	ivec3 size3 = textureSize(u_macro_elevation, 0);
	float texture_res = float(size3.x);
	float gutter = max((texture_res - u_macro_face_res) * 0.5, 0.0);
	vec2 uv = (fuv.xy * u_macro_face_res + vec2(gutter)) / texture_res;
	return vec3(uv, fuv.z);
}

float macro_lod_for_spacing(float spacing_m) {
	float macro_texel_m = (PI * 0.5 * u_planet_radius) / max(u_macro_face_res, 1.0);
	float footprint_m = max(spacing_m * MACRO_FILTER_FOOTPRINT_VERTICES, macro_texel_m);
	return clamp(log2(footprint_m / macro_texel_m), 0.0, MACRO_MAX_LOD);
}

vec4 cubic_bspline_weights(float t) {
	float t2 = t * t;
	float t3 = t2 * t;
	return vec4(
		(1.0 - 3.0 * t + 3.0 * t2 - t3) / 6.0,
		(4.0 - 6.0 * t2 + 3.0 * t3) / 6.0,
		(1.0 + 3.0 * t + 3.0 * t2 - 3.0 * t3) / 6.0,
		t3 / 6.0);
}

float macro_bicubic_mip(vec3 coord, int mip) {
	ivec3 size3 = textureSize(u_macro_elevation, mip);
	vec2 size2 = vec2(size3.xy);
	vec2 p = coord.xy * size2 - vec2(0.5);
	vec2 base = floor(p);
	vec2 f = fract(p);

	vec4 wx = cubic_bspline_weights(f.x);
	vec4 wy = cubic_bspline_weights(f.y);
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

float macro_cubic_trilinear(vec3 coord, float lod) {
	int mip0 = clamp(int(floor(lod)), 0, MACRO_MAX_MIP);
	int mip1 = min(mip0 + 1, MACRO_MAX_MIP);
	float t = fract(lod);
	float h0 = macro_bicubic_mip(coord, mip0);
	if (mip0 == mip1 || t <= 0.001) {
		return h0;
	}
	float h1 = macro_bicubic_mip(coord, mip1);
	return mix(h0, h1, smootherstep01(t));
}

float macro_height(vec3 d, int level) {
	if (u_macro_ready < 0.5 || u_macro_face_res <= 0.5) {
		return 0.0;
	}
	float lod = macro_lod_for_spacing(ring_spacing(level));
	return macro_cubic_trilinear(macro_array_coord(d), lod);
}

uint terrain_hash(ivec3 p, uint seed) {
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

float lattice_value(ivec3 p, uint seed) {
	uint h = terrain_hash(p, seed) & 0x00ffffffu;
	return float(h) * (2.0 / 16777215.0) - 1.0;
}

float smooth_value_noise(vec3 p, uint seed) {
	ivec3 i = ivec3(floor(p));
	vec3 f = fract(p);
	f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	float n000 = lattice_value(i + ivec3(0, 0, 0), seed);
	float n100 = lattice_value(i + ivec3(1, 0, 0), seed);
	float n010 = lattice_value(i + ivec3(0, 1, 0), seed);
	float n110 = lattice_value(i + ivec3(1, 1, 0), seed);
	float n001 = lattice_value(i + ivec3(0, 0, 1), seed);
	float n101 = lattice_value(i + ivec3(1, 0, 1), seed);
	float n011 = lattice_value(i + ivec3(0, 1, 1), seed);
	float n111 = lattice_value(i + ivec3(1, 1, 1), seed);
	float nx00 = mix(n000, n100, f.x);
	float nx10 = mix(n010, n110, f.x);
	float nx01 = mix(n001, n101, f.x);
	float nx11 = mix(n011, n111, f.x);
	return mix(mix(nx00, nx10, f.y), mix(nx01, nx11, f.y), f.z);
}

float band_weight(float wavelength_m, float spacing_m) {
	return smootherstep_range(spacing_m * 4.0, spacing_m * 8.0, wavelength_m);
}

float detail_band(vec3 world_m, float wavelength_m, float spacing_m,
		uint seed, float amplitude_m) {
	float w = band_weight(wavelength_m, spacing_m);
	if (w <= 0.001) {
		return 0.0;
	}
	return smooth_value_noise(world_m / wavelength_m, seed) * amplitude_m * w;
}

float procedural_detail(vec3 d, int level, float macro_h) {
	float spacing = ring_spacing(level);
	vec3 world_m = d * u_planet_radius;
	uint seed = uint(max(u_detail_seed, 1));
	float coast_guard = mix(0.38, 1.0,
		smootherstep_range(45.0, 320.0, abs(macro_h)));
	float mountain = smootherstep_range(500.0, 2600.0, max(macro_h, 0.0));
	float regional_gain = mix(0.72, 1.30, mountain);
	float h = 0.0;
	h += detail_band(world_m, 12000.0, spacing, seed + 11u,
		mix(52.0, 115.0, mountain));
	float w2400 = band_weight(2400.0, spacing);
	if (w2400 > 0.001) {
		float n = smooth_value_noise(world_m / 2400.0, seed + 23u);
		float ridge = (1.0 - abs(n)) * 2.0 - 1.0;
		h += (n * 28.0 + ridge * 18.0 * mountain) * w2400;
	}
	h += detail_band(world_m, 480.0, spacing, seed + 37u,
		mix(7.0, 15.0, mountain));
	h += detail_band(world_m, 96.0, spacing, seed + 53u, 3.4);
	h += detail_band(world_m, 24.0, spacing, seed + 71u, 0.85);
	return h * coast_guard * regional_gain * u_detail_strength;
}

float terrain_height_for_level(vec3 d, int level) {
	float macro_h = macro_height(d, level);
	return macro_h + procedural_detail(d, level, macro_h);
}

float fine_morph_weight(vec2 cell) {
	float half_cells = u_grid_cells * 0.5;
	float q = length(cell) / max(half_cells, 1.0);
	return smootherstep_range(FINE_MORPH_START_Q, 1.0, q);
}

float coarse_sink_weight(vec2 cell, int level, float is_ring) {
	// A promoted centre disc may logically be L5 or L13, but it is still the
	// centre disc and has no finer layer underneath it. Only annular instances
	// receive the radial sink used to hide overlap with their finer neighbour.
	if (is_ring < 0.5 || level <= 0) {
		return 0.0;
	}
	float half_cells = u_grid_cells * 0.5;
	float q = length(cell) / max(half_cells, 1.0);
	return 1.0 - smootherstep_range(RING_INNER_Q, RING_HANDOFF_Q, q);
}

float terrain_height_with_morph(vec3 d, int level, float morph) {
	float fine = terrain_height_for_level(d, level);
	if (level < ASTERRA_MAX_LEVEL && morph > 0.001) {
		float coarse = terrain_height_for_level(d, level + 1);
		fine = mix(fine, coarse, morph);
	}
	return fine;
}

float macro_height_with_morph(vec3 d, int level, float morph) {
	float fine = macro_height(d, level);
	if (level < ASTERRA_MAX_LEVEL && morph > 0.001) {
		float coarse = macro_height(d, level + 1);
		fine = mix(fine, coarse, morph);
	}
	return fine;
}

vec3 geodesic_offset(vec3 d, vec3 tangent, float distance_m) {
	float theta = distance_m / max(u_planet_radius, 1.0);
	return normalize(d * cos(theta) + tangent * sin(theta));
}

vec3 smooth_macro_normal(vec3 d, int level, float morph, float center_macro_h) {
	// This basis depends only on the planet-space direction, not the clipmap
	// anchor. Reanchoring therefore cannot rotate the normal sampling stencil.
	vec3 reference_axis = abs(d.y) < 0.95
		? vec3(0.0, 1.0, 0.0)
		: vec3(1.0, 0.0, 0.0);
	vec3 tangent_x = normalize(cross(reference_axis, d));
	vec3 tangent_y = normalize(cross(d, tangent_x));
	float sample_m = max(NORMAL_SAMPLE_MIN_M,
		ring_spacing(level) * NORMAL_SAMPLE_SPACING_MULT);

	vec3 dx = geodesic_offset(d, tangent_x, sample_m);
	vec3 dy = geodesic_offset(d, tangent_y, sample_m);
	float hx = macro_height_with_morph(dx, level, morph);
	float hy = macro_height_with_morph(dy, level, morph);

	vec3 p0 = d * (u_planet_radius + center_macro_h);
	vec3 px = dx * (u_planet_radius + hx);
	vec3 py = dy * (u_planet_radius + hy);
	vec3 n_raw = cross(px - p0, py - p0);
	vec3 n = length(n_raw) > 1e-8 ? normalize(n_raw) : d;
	if (dot(n, d) < 0.0) {
		n = -n;
	}
	return n;
}

void vertex() {
	vec2 cell = UV - vec2(u_grid_cells * 0.5);
	int level = int(floor(INSTANCE_CUSTOM.x + 0.5));
	float is_ring = INSTANCE_CUSTOM.z;
	float spacing = ring_spacing(level);
	vec2 offset_m = cell * spacing;
	float raw_theta = length(offset_m) / max(u_planet_radius, 1.0);
	vec3 d = direction_for_offset(offset_m);
	float morph = fine_morph_weight(cell);
	float macro_h = macro_height_with_morph(d, level, morph);
	float h = terrain_height_with_morph(d, level, morph);
	float sink = coarse_sink_weight(cell, level, is_ring) * max(spacing * u_sink_scale, 0.5);
	float height = (u_height_enabled > 0.5 ? h : 0.0) - sink;
	vec3 p = d * (u_planet_radius + height + u_surface_bias);

	v_planet = p;
	v_render = p - u_origin;
	v_smooth_normal = u_height_enabled > 0.5
		? smooth_macro_normal(d, level, morph, macro_h)
		: d;
	v_valid = u_macro_ready;
	v_cap_valid = raw_theta <= u_visible_cap_angle ? 1.0 : 0.0;
	v_level = float(level);

	// Cloud shadowing is evaluated on the geometry lattice and interpolated across
	// triangles. The density field is kilometre-scale, while even coarse visible
	// rings have far more spatial resolution than the shadow needs. This avoids a
	// multi-sample 3D raymarch in every 4K screen pixel.
	vec3 sun_dir = normalize(u_cloud_shadow_sun_dir);
	float planet_vis = planet_direct_sun_visibility(p, sun_dir, u_planet_radius);
	v_direct_transmittance = planet_vis;
	if (planet_vis > 0.001) {
		v_direct_transmittance *= cloud_shadow_transmittance(
			p, sun_dir, u_planet_radius);
	}
	VERTEX = v_render;
}

void fragment() {
	if (v_valid < 0.5 || v_cap_valid < 0.5) {
		discard;
	}
	if (u_debug_side_cut > 0.5) {
		vec3 planet_dir = normalize(v_planet);
		if (dot(planet_dir, u_center_up) > 0.0) {
			discard;
		}
	}

	vec3 up = normalize(v_planet);
	vec3 wn = normalize(v_smooth_normal);
	if (dot(wn, up) < 0.0) {
		wn = -wn;
	}
	NORMAL = normalize((VIEW_MATRIX * vec4(wn, 0.0)).xyz);

	if (u_debug_side_cut > 0.5) {
		float phase = v_level * 0.13;
		vec3 lod_color = 0.5 + 0.5 * cos(TAU * (vec3(phase) + vec3(0.0, 0.33, 0.67)));
		ALBEDO = mix(vec3(0.06), lod_color, 0.92);
		EMISSION = lod_color * 0.10;
		ROUGHNESS = 1.0;
		SPECULAR = 0.0;
	} else {
		float slope = clamp(1.0 - dot(wn, up), 0.0, 1.0);
		vec4 surf = vec4(0.38, 0.38, 0.12, 0.0);
		if (v_level <= 8.5) {
			surf = material_clipmap_surface(v_planet, u_planet_radius, surf);
		}
		vec3 soil = vec3(0.24, 0.17, 0.105);
		vec3 grass = vec3(0.17, 0.255, 0.095);
		vec3 wet = vec3(0.12, 0.145, 0.085);
		vec3 sand = vec3(0.48, 0.39, 0.23);
		vec3 rock = vec3(0.285, 0.275, 0.255);
		vec3 snow = vec3(0.78, 0.80, 0.81);
		vec3 albedo = mix(soil, grass, clamp(surf.r, 0.0, 1.0));
		albedo = mix(albedo, wet, clamp(surf.g * 0.70, 0.0, 1.0));
		albedo = mix(albedo, sand, clamp(surf.b, 0.0, 1.0));
		albedo = mix(albedo, snow, clamp(surf.a, 0.0, 1.0));
		albedo = mix(albedo, rock, smoothstep(0.08, 0.34, slope));
		ALBEDO = albedo;
		ROUGHNESS = mix(0.94, 0.72, smoothstep(0.10, 0.42, slope));
		SPECULAR = 0.22;
	}
}

void light() {
	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	if (ndotl > 0.0) {
		float visibility = LIGHT_IS_DIRECTIONAL ? clamp(v_direct_transmittance, 0.0, 1.0) : 1.0;
		vec3 radiance = LIGHT_COLOR * ATTENUATION * visibility;

		float ndotv = max(dot(NORMAL, VIEW), 0.001);
		vec3 h = normalize(LIGHT + VIEW);
		float ndoth = max(dot(NORMAL, h), 0.0);
		float vdoth = max(dot(VIEW, h), 0.0);
		float rough = clamp(ROUGHNESS, 0.045, 1.0);

		float fd90 = 0.5 + 2.0 * vdoth * vdoth * rough;
		float fl = pow(1.0 - ndotl, 5.0);
		float fv = pow(1.0 - ndotv, 5.0);
		float diffuse_term = (1.0 + (fd90 - 1.0) * fl)
			* (1.0 + (fd90 - 1.0) * fv);
		DIFFUSE_LIGHT += radiance * ndotl * diffuse_term / PI;

		if (u_debug_side_cut <= 0.5) {
			float alpha = rough * rough;
			float a2 = alpha * alpha;
			float denom = ndoth * ndoth * (a2 - 1.0) + 1.0;
			float d = a2 / max(PI * denom * denom, 1e-5);
			float k = (rough + 1.0) * (rough + 1.0) * 0.125;
			float gv = ndotv / max(ndotv * (1.0 - k) + k, 1e-4);
			float gl = ndotl / max(ndotl * (1.0 - k) + k, 1e-4);
			float f0 = 0.018;
			float fresnel = f0 + (1.0 - f0) * pow(1.0 - vdoth, 5.0);
			float spec = d * gv * gl * fresnel
				/ max(4.0 * ndotv * ndotl, 1e-4);
			SPECULAR_LIGHT += radiance * ndotl * spec * SPECULAR_AMOUNT;
		}
	}
}
