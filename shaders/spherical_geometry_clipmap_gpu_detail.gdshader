shader_type spatial;
render_mode cull_disabled, diffuse_burley, specular_schlick_ggx;

#include "res://shaders/material_clipmap.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"

uniform sampler2D u_height_page_atlas : filter_nearest, repeat_disable;
uniform sampler2D u_height_page_table : filter_nearest, repeat_disable;
uniform sampler2D u_height_page_birth_table : filter_nearest, repeat_disable;
uniform float u_page_atlas_ready = 0.0;
uniform float u_page_birth_ready = 0.0;
uniform float u_page_table_capacity = 8192.0;
uniform float u_page_table_probes = 12.0;
uniform float u_page_atlas_cols = 64.0;
uniform float u_page_tile_cells = 32.0;
uniform float u_page_tile_verts = 33.0;
uniform float u_finest_cells = 2097152.0;
uniform float u_stream_time_s = 0.0;
uniform float u_page_fade_seconds = 0.70;

uniform sampler2DArray u_orbit_elevation : filter_linear, repeat_disable;
uniform float u_orbit_ready = 0.0;
uniform float u_orbit_face_res = 0.0;

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

// GPU-only visual terrain refinement. L0-L3 never wait for height pages: they
// start from the 2x macro/orbit field and add deterministic world-space residuals.
uniform vec3 u_gpu_detail_seed = vec3(1.7, 3.1, 5.9);
uniform float u_gpu_detail_strength = 1.0;

varying vec3 v_planet;
varying vec3 v_render;
varying float v_valid;
varying float v_cap_valid;
varying float v_level;

const float ASTERRA_Q = 0.7853981633974483;
const int ASTERRA_MAX_LEVEL = 14;
const int ASTERRA_GPU_PROC_MAX_LEVEL = 3;
const int ASTERRA_SPARSE_MIN_LEVEL = 4;
const int ASTERRA_SPARSE_MAX_LEVEL = 6;
const int ASTERRA_MAX_TABLE_PROBES = 12;
const int ASTERRA_SPARSE_FALLBACK_STEPS = 3;
const float RING_INNER_Q = 0.44;
const float RING_HANDOFF_Q = 0.50;
const float FINE_MORPH_START_Q = 0.875;
const float MAX_PROJECTED_THETA = 1.62;
const float TAU_F = 6.28318530718;

float ring_spacing(int level) {
	return u_base_spacing * exp2(float(level));
}

vec3 direction_for_offset(vec2 offset_m, int level) {
	float arc = length(offset_m);
	if (arc < 1e-5) {
		return normalize(u_center_dir);
	}
	if (level <= ASTERRA_SPARSE_MAX_LEVEL) {
		return normalize(u_center_dir
			+ u_center_right * (offset_m.x / u_planet_radius)
			+ u_center_up * (offset_m.y / u_planet_radius));
	}
	float raw_theta = arc / max(u_planet_radius, 1.0);
	float theta = min(raw_theta, min(u_visible_cap_angle * 1.02, MAX_PROJECTED_THETA));
	vec3 tangent = normalize(u_center_right * offset_m.x + u_center_up * offset_m.y);
	return normalize(u_center_dir * cos(theta) + tangent * sin(theta));
}

// Smooth trigonometric 3D noise is deliberately used here instead of a hash-grid
// value noise: it is continuous across cube faces, uses no page addressing, and
// costs only a few ALU operations per band. All coordinates are planet-space so
// reanchoring the clipmap never changes the generated landform.
float gpu_smooth_noise(vec3 p, vec3 phase) {
	vec3 q = p + phase + u_gpu_detail_seed;
	float a = sin(q.x + sin(q.y * 0.73 + q.z * 0.21));
	float b = sin(q.y * 1.13 + sin(q.z * 0.81 + q.x * 0.19));
	float c = sin(q.z * 0.91 + sin(q.x * 0.67 + q.y * 0.27));
	return (a + b + c) * 0.3333333333;
}

float gpu_band(vec3 world_m, float wavelength_m, vec3 phase) {
	return gpu_smooth_noise(world_m * (TAU_F / wavelength_m), phase);
}

float gpu_band_weight(float wavelength_m, int level) {
	float spacing = ring_spacing(level);
	// Mesh detail needs several vertices per wavelength. This broad roll-off makes
	// every child a strict refinement of its parent and avoids a new procedural LOD pop.
	return smoothstep(spacing * 3.5, spacing * 7.0, wavelength_m);
}

float gpu_procedural_residual(vec3 d, int level, float macro_h) {
	vec3 world_m = d * u_planet_radius;
	float shore_guard = mix(0.30, 1.0, smoothstep(40.0, 260.0, abs(macro_h)));
	float relief_gain = mix(0.72, 1.12, smoothstep(250.0, 2600.0, abs(macro_h)));
	float gain = u_gpu_detail_strength * shore_guard * relief_gain;

	float h = 0.0;
	float n0 = gpu_band(world_m, 1800.0, vec3(0.7, 1.9, 4.1));
	float ridge0 = (1.0 - abs(gpu_band(world_m, 940.0, vec3(5.2, 0.4, 2.8)))) * 2.0 - 1.0;
	h += n0 * 38.0 * gpu_band_weight(1800.0, level);
	h += ridge0 * 17.0 * gpu_band_weight(940.0, level);
	h += gpu_band(world_m, 360.0, vec3(2.1, 5.7, 1.3)) * 8.0
		* gpu_band_weight(360.0, level);
	h += gpu_band(world_m, 112.0, vec3(4.8, 2.4, 6.0)) * 2.7
		* gpu_band_weight(112.0, level);
	h += gpu_band(world_m, 34.0, vec3(1.1, 3.6, 5.5)) * 0.85
		* gpu_band_weight(34.0, level);
	h += gpu_band(world_m, 10.0, vec3(6.1, 1.5, 3.2)) * 0.22
		* gpu_band_weight(10.0, level);
	return h * gain;
}

vec3 face_uv_for_dir(vec3 d) {
	float ax = abs(d.x);
	float ay = abs(d.y);
	float az = abs(d.z);
	int face;
	vec3 axis;
	vec3 right;
	vec3 up;
	if (ax >= ay && ax >= az) {
		if (d.x > 0.0) {
			face = 0; axis = vec3(1.0, 0.0, 0.0); right = vec3(0.0, 0.0, -1.0);
		} else {
			face = 1; axis = vec3(-1.0, 0.0, 0.0); right = vec3(0.0, 0.0, 1.0);
		}
		up = vec3(0.0, 1.0, 0.0);
	} else if (ay >= az) {
		if (d.y > 0.0) {
			face = 2; axis = vec3(0.0, 1.0, 0.0); up = vec3(0.0, 0.0, 1.0);
		} else {
			face = 3; axis = vec3(0.0, -1.0, 0.0); up = vec3(0.0, 0.0, -1.0);
		}
		right = vec3(-1.0, 0.0, 0.0);
	} else {
		if (d.z > 0.0) {
			face = 4; axis = vec3(0.0, 0.0, 1.0); right = vec3(1.0, 0.0, 0.0);
		} else {
			face = 5; axis = vec3(0.0, 0.0, -1.0); right = vec3(-1.0, 0.0, 0.0);
		}
		up = vec3(0.0, 1.0, 0.0);
	}
	float a = max(dot(axis, d), 1e-12);
	float u = atan(dot(right, d) / a) / ASTERRA_Q;
	float v = atan(dot(up, d) / a) / ASTERRA_Q;
	return vec3(float(face), u, v);
}

int page_hash(int page_code, int tile_x, int tile_y) {
	int capacity = int(u_page_table_capacity + 0.5);
	int h = tile_x * 1973 + tile_y * 9277 + page_code * 26699;
	int result = h % capacity;
	return result < 0 ? result + capacity : result;
}

ivec2 page_lookup(int level, int face, int tile_x, int tile_y) {
	int capacity = int(u_page_table_capacity + 0.5);
	int probe_limit = min(int(u_page_table_probes + 0.5), ASTERRA_MAX_TABLE_PROBES);
	int page_code = level * 6 + face + 1;
	int start = page_hash(page_code, tile_x, tile_y);
	for (int probe = 0; probe < ASTERRA_MAX_TABLE_PROBES; probe++) {
		if (probe >= probe_limit) {
			break;
		}
		int table_index = (start + probe) % capacity;
		vec4 meta = texelFetch(u_height_page_table, ivec2(table_index, 0), 0);
		int stored_slot = int(floor(meta.a + 0.5));
		if (stored_slot == 0) {
			return ivec2(-1, -1);
		}
		if (stored_slot < 0) {
			continue;
		}
		if (int(floor(meta.r + 0.5)) == page_code
				&& int(floor(meta.g + 0.5)) == tile_x
				&& int(floor(meta.b + 0.5)) == tile_y) {
			return ivec2(stored_slot - 1, table_index);
		}
	}
	return ivec2(-1, -1);
}

float page_refinement_weight(int table_index) {
	if (u_page_birth_ready < 0.5 || table_index < 0) {
		return 1.0;
	}
	float birth = texelFetch(u_height_page_birth_table, ivec2(table_index, 0), 0).r;
	if (birth <= 0.0) {
		return 1.0;
	}
	float duration = max(u_page_fade_seconds, 0.001);
	float t = clamp((u_stream_time_s - birth) / duration, 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

ivec2 tile_address_for_vertex(int ix, int iy, int cells) {
	int tile_cells = int(u_page_tile_cells + 0.5);
	int tile_count = (cells + tile_cells - 1) / tile_cells;
	return ivec2(
		clamp((max(ix, 1) - 1) / tile_cells, 0, tile_count - 1),
		clamp((max(iy, 1) - 1) / tile_cells, 0, tile_count - 1));
}

float atlas_height(int slot, int local_x, int local_y) {
	int atlas_cols = int(u_page_atlas_cols + 0.5);
	int tile_verts = int(u_page_tile_verts + 0.5);
	int page_x = slot % atlas_cols;
	int page_y = slot / atlas_cols;
	ivec2 pixel = ivec2(page_x * tile_verts + local_x, page_y * tile_verts + local_y);
	return texelFetch(u_height_page_atlas, pixel, 0).r;
}

vec3 height_vertex(int level, int face, int ix, int iy, int cells) {
	int tile_cells = int(u_page_tile_cells + 0.5);
	ivec2 tile = tile_address_for_vertex(ix, iy, cells);
	ivec2 lookup = page_lookup(level, face, tile.x, tile.y);
	if (lookup.x < 0) {
		return vec3(0.0, 0.0, 0.0);
	}
	int local_x = ix - tile.x * tile_cells;
	int local_y = iy - tile.y * tile_cells;
	return vec3(atlas_height(lookup.x, local_x, local_y), 1.0,
		page_refinement_weight(lookup.y));
}

vec3 sample_exact_level(vec3 d, int level) {
	vec3 fuv = face_uv_for_dir(d);
	int face = int(floor(fuv.x + 0.5));
	int cells = int(floor(u_finest_cells / exp2(float(level)) + 0.5));
	float fx = clamp((fuv.y * 0.5 + 0.5) * float(cells), 0.0, float(cells));
	float fy = clamp((fuv.z * 0.5 + 0.5) * float(cells), 0.0, float(cells));
	int x0 = int(floor(fx));
	int y0 = int(floor(fy));
	int x1 = min(x0 + 1, cells);
	int y1 = min(y0 + 1, cells);
	float tx = fx - float(x0);
	float ty = fy - float(y0);
	int tile_cells = int(u_page_tile_cells + 0.5);

	ivec2 tile0 = tile_address_for_vertex(x0, y0, cells);
	ivec2 tile1 = tile_address_for_vertex(x1, y1, cells);
	if (tile0.x == tile1.x && tile0.y == tile1.y) {
		ivec2 lookup = page_lookup(level, face, tile0.x, tile0.y);
		if (lookup.x < 0) {
			return vec3(0.0, 0.0, 0.0);
		}
		int lx0 = x0 - tile0.x * tile_cells;
		int ly0 = y0 - tile0.y * tile_cells;
		int lx1 = x1 - tile0.x * tile_cells;
		int ly1 = y1 - tile0.y * tile_cells;
		float h00 = atlas_height(lookup.x, lx0, ly0);
		float h10 = atlas_height(lookup.x, lx1, ly0);
		float h01 = atlas_height(lookup.x, lx0, ly1);
		float h11 = atlas_height(lookup.x, lx1, ly1);
		return vec3(mix(mix(h00, h10, tx), mix(h01, h11, tx), ty), 1.0,
			page_refinement_weight(lookup.y));
	}

	vec3 h00 = height_vertex(level, face, x0, y0, cells);
	vec3 h10 = height_vertex(level, face, x1, y0, cells);
	vec3 h01 = height_vertex(level, face, x0, y1, cells);
	vec3 h11 = height_vertex(level, face, x1, y1, cells);
	if (min(min(h00.y, h10.y), min(h01.y, h11.y)) < 0.5) {
		return vec3(0.0, 0.0, 0.0);
	}
	float fade = min(min(h00.z, h10.z), min(h01.z, h11.z));
	return vec3(mix(mix(h00.x, h10.x, tx), mix(h01.x, h11.x, tx), ty), 1.0, fade);
}

vec2 orbit_fallback(vec3 d) {
	if (u_orbit_ready < 0.5 || u_orbit_face_res <= 0.5) {
		return vec2(0.0, 0.0);
	}
	vec3 coord = surface_array_coord(d, u_orbit_face_res);
	return vec2(texture(u_orbit_elevation, coord).r, 1.0);
}

vec2 raw_parent_height(vec3 d, int child_level) {
	int parent_level = child_level + 1;
	if (parent_level >= ASTERRA_SPARSE_MIN_LEVEL
			&& parent_level <= ASTERRA_SPARSE_MAX_LEVEL
			&& u_page_atlas_ready > 0.5) {
		vec3 parent = sample_exact_level(d, parent_level);
		if (parent.y > 0.5) {
			return vec2(parent.x, 1.0);
		}
	}
	return orbit_fallback(d);
}

vec2 sample_sparse_level(vec3 d, int requested_level) {
	if (u_page_atlas_ready > 0.5) {
		for (int step = 0; step < ASTERRA_SPARSE_FALLBACK_STEPS; step++) {
			int candidate = requested_level + step;
			if (candidate > ASTERRA_SPARSE_MAX_LEVEL) {
				break;
			}
			vec3 result = sample_exact_level(d, candidate);
			if (result.y > 0.5) {
				float height = result.x;
				if (result.z < 0.999) {
					vec2 parent = raw_parent_height(d, candidate);
					if (parent.y > 0.5) {
						height = mix(parent.x, result.x, result.z);
					}
				}
				return vec2(height, 1.0);
			}
		}
	}
	return orbit_fallback(d);
}

vec2 sample_best_level(vec3 d, int requested_level) {
	if (requested_level <= ASTERRA_GPU_PROC_MAX_LEVEL) {
		vec2 macro = orbit_fallback(d);
		if (macro.y < 0.5) {
			return macro;
		}
		return vec2(macro.x + gpu_procedural_residual(d, requested_level, macro.x), 1.0);
	}
	if (requested_level <= ASTERRA_SPARSE_MAX_LEVEL) {
		return sample_sparse_level(d, max(requested_level, ASTERRA_SPARSE_MIN_LEVEL));
	}
	return orbit_fallback(d);
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

vec2 clip_height(vec3 d, vec2 cell, int level) {
	if (level > ASTERRA_SPARSE_MAX_LEVEL) {
		return orbit_fallback(d);
	}
	vec2 fine = sample_best_level(d, level);
	if (fine.y < 0.5) {
		return fine;
	}
	float morph = fine_morph_weight(cell);
	if (level < ASTERRA_MAX_LEVEL && morph > 0.001) {
		vec2 coarse = sample_best_level(d, level + 1);
		if (coarse.y > 0.5) {
			fine.x = mix(fine.x, coarse.x, morph);
		}
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
	vec2 h = clip_height(d, cell, level);
	float sink = coarse_sink_weight(cell, level) * max(spacing * u_sink_scale, 0.5);
	float height = (u_height_enabled > 0.5 ? h.x : 0.0) - sink;
	vec3 p = d * (u_planet_radius + height + u_surface_bias);

	v_planet = p;
	v_render = p - u_origin;
	v_valid = h.y;
	v_cap_valid = raw_theta <= u_visible_cap_angle ? 1.0 : 0.0;
	v_level = float(level);
	VERTEX = v_render;
}

void fragment() {
	if (v_valid < 0.5 || v_cap_valid < 0.5) {
		discard;
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
