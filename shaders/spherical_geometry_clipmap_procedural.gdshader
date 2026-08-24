shader_type spatial;
render_mode cull_disabled, diffuse_burley, specular_schlick_ggx;

#include "res://shaders/material_clipmap.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"

// One smooth low-frequency source for the entire planet. The texture is built
// from the runtime-smoothed macro elevation at 2x face resolution and sampled
// with hardware interpolation, so macro cells never appear as pixels/blocks.
uniform sampler2DArray u_macro_elevation : filter_linear, repeat_disable;
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

// Inspection mode. The plane through (planet centre, clipmap centre,
// clipmap-right) removes the +clipmap-up half of the terrain. Looking along the
// clipmap-up direction exposes a true radial profile of every overlapping ring.
uniform float u_debug_side_cut = 0.0;

varying vec3 v_planet;
varying vec3 v_render;
varying float v_valid;
varying float v_cap_valid;
varying float v_level;

const int ASTERRA_MAX_LEVEL = 14;
const float RING_INNER_Q = 0.44;
const float RING_HANDOFF_Q = 0.50;
const float FINE_MORPH_START_Q = 0.875;
const float MAX_PROJECTED_THETA = 1.62;

float ring_spacing(int level) {
	return u_base_spacing * exp2(float(level));
}

vec3 direction_for_offset(vec2 offset_m, int level) {
	float arc = length(offset_m);
	if (arc < 1e-5) {
		return normalize(u_center_dir);
	}

	// Near and regional rings are small enough that the normalized tangent form
	// is visually indistinguishable from the exponential map and much cheaper.
	if (level <= 8) {
		return normalize(u_center_dir
			+ u_center_right * (offset_m.x / u_planet_radius)
			+ u_center_up * (offset_m.y / u_planet_radius));
	}

	float raw_theta = arc / max(u_planet_radius, 1.0);
	float theta = min(raw_theta, min(u_visible_cap_angle * 1.02, MAX_PROJECTED_THETA));
	vec3 tangent = normalize(u_center_right * offset_m.x + u_center_up * offset_m.y);
	return normalize(u_center_dir * cos(theta) + tangent * sin(theta));
}

float macro_height(vec3 d) {
	if (u_macro_ready < 0.5 || u_macro_face_res <= 0.5) {
		return 0.0;
	}
	vec3 coord = surface_array_coord(d, u_macro_face_res);
	return texture(u_macro_elevation, coord).r;
}

// Integer lattice hash: no texture pages and no transcendental hash. The input is
// planet-space, so the function is continuous across cube-face boundaries and is
// deterministic at every visit/reanchor.
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
	f = f * f * (3.0 - 2.0 * f);

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
	// A wide transition is deliberate. Each LOD progressively loses a band rather
	// than aliasing it, so the outer morph always has a nearby parent to land on.
	return smoothstep(spacing_m * 4.0, spacing_m * 8.0, wavelength_m);
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

	// Detail is reduced around the sea-level crossing so the low-resolution macro
	// shoreline remains stable while still allowing rocky coasts away from zero.
	float coast_guard = mix(0.38, 1.0, smoothstep(45.0, 320.0, abs(macro_h)));
	float mountain = smoothstep(500.0, 2600.0, max(macro_h, 0.0));
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
	float macro_h = macro_height(d);
	return macro_h + procedural_detail(d, level, macro_h);
}

float fine_morph_weight(vec2 cell) {
	float half_cells = u_grid_cells * 0.5;
	float q = length(cell) / max(half_cells, 1.0);
	return smoothstep(FINE_MORPH_START_Q, 1.0, q);
}

float coarse_sink_weight(vec2 cell, int level) {
	if (level <= 0) {
		return 0.0;
	}
	float half_cells = u_grid_cells * 0.5;
	float q = length(cell) / max(half_cells, 1.0);
	return 1.0 - smoothstep(RING_INNER_Q, RING_HANDOFF_Q, q);
}

float clip_height(vec3 d, vec2 cell, int level) {
	float fine = terrain_height_for_level(d, level);
	float morph = fine_morph_weight(cell);
	if (level < ASTERRA_MAX_LEVEL && morph > 0.001) {
		float coarse = terrain_height_for_level(d, level + 1);
		fine = mix(fine, coarse, morph);
	}
	return fine;
}

void vertex() {
	int grid_verts = int(u_grid_cells + 1.5);
	int grid_x = VERTEX_ID % grid_verts;
	int grid_y = VERTEX_ID / grid_verts;
	vec2 cell = vec2(float(grid_x), float(grid_y)) - vec2(u_grid_cells * 0.5);
	int level = int(floor(INSTANCE_CUSTOM.x + 0.5));
	float spacing = ring_spacing(level);
	vec2 offset_m = cell * spacing;
	float raw_theta = length(offset_m) / max(u_planet_radius, 1.0);
	vec3 d = direction_for_offset(offset_m, level);
	float h = clip_height(d, cell, level);
	float sink = coarse_sink_weight(cell, level) * max(spacing * u_sink_scale, 0.5);
	float height = (u_height_enabled > 0.5 ? h : 0.0) - sink;
	vec3 p = d * (u_planet_radius + height + u_surface_bias);

	v_planet = p;
	v_render = p - u_origin;
	v_valid = u_macro_ready;
	v_cap_valid = raw_theta <= u_visible_cap_angle ? 1.0 : 0.0;
	v_level = float(level);
	VERTEX = v_render;
}

void fragment() {
	if (v_valid < 0.5 || v_cap_valid < 0.5) {
		discard;
	}

	// A half-plane rather than a narrow strip is deliberate: it leaves a clean
	// cut edge through the centre while preserving enough triangles to inspect the
	// overlapping annuli in wireframe. The cut plane rotates with the frozen
	// clipmap frame, not with the camera.
	if (u_debug_side_cut > 0.5) {
		vec3 planet_dir = normalize(v_planet);
		if (dot(planet_dir, u_center_up) > 0.0) {
			discard;
		}
	}

	vec3 up = normalize(v_planet);
	vec3 dx = dFdx(v_render);
	vec3 dy = dFdy(v_render);
	vec3 wn_raw = cross(dx, dy);
	vec3 wn = length(wn_raw) > 1e-7 ? normalize(wn_raw) : up;
	if (dot(wn, up) < 0.0) {
		wn = -wn;
	}
	NORMAL = normalize((VIEW_MATRIX * vec4(wn, 0.0)).xyz);

	if (u_debug_side_cut > 0.5) {
		// Automatic LOD colouring makes nested/sunk surfaces distinguishable even
		// without global wireframe mode.
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
