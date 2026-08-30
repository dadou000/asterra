shader_type spatial;
render_mode cull_disabled, diffuse_burley, specular_schlick_ggx;

#include "res://shaders/material_clipmap.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"

// Planet-wide sparse GPU height cache.
uniform sampler2D u_height_page_atlas : filter_nearest, repeat_disable;
uniform sampler2D u_height_page_table : filter_nearest, repeat_disable;
uniform float u_page_atlas_ready = 0.0;
uniform vec2 u_page_atlas_size = vec2(2112.0, 2112.0);
uniform float u_page_table_capacity = 16384.0;
uniform float u_page_table_probes = 32.0;
uniform float u_page_atlas_cols = 64.0;
uniform float u_page_tile_cells = 32.0;
uniform float u_page_tile_verts = 33.0;
uniform float u_finest_cells = 2097152.0;

// Immediate safety fallback while sparse pages sharpen.
uniform sampler2DArray u_orbit_elevation : filter_nearest, repeat_disable;
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

varying vec3 v_planet;
varying vec3 v_render;
varying float v_valid;
varying float v_cap_valid;

const float ASTERRA_Q = 0.7853981633974483;
const int ASTERRA_MAX_LEVEL = 14;
const int ASTERRA_MAX_TABLE_PROBES = 32;
const float RING_INNER_Q = 0.44;
const float RING_HANDOFF_Q = 0.50;
const float FINE_MORPH_START_Q = 0.875;
const float MAX_PROJECTED_THETA = 1.62;

float ring_spacing(int level) {
	return u_base_spacing * exp2(float(level));
}

vec3 direction_for_offset(vec2 offset_m) {
	float arc = length(offset_m);
	if (arc < 1e-5) {
		return normalize(u_center_dir);
	}
	// The largest square rings extend beyond the visible spherical cap. Never let
	// those unused vertices wrap around the back side of the sphere: collapse them
	// just outside the cap and let v_cap_valid discard their fragments.
	float raw_theta = arc / max(u_planet_radius, 1.0);
	float theta = min(raw_theta, min(u_visible_cap_angle * 1.02, MAX_PROJECTED_THETA));
	vec3 tangent = normalize(u_center_right * offset_m.x + u_center_up * offset_m.y);
	return normalize(u_center_dir * cos(theta) + tangent * sin(theta));
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

int page_slot(int level, int face, int tile_x, int tile_y) {
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
			return -1;
		}
		if (stored_slot < 0) {
			continue;
		}
		if (int(floor(meta.r + 0.5)) == page_code
				&& int(floor(meta.g + 0.5)) == tile_x
				&& int(floor(meta.b + 0.5)) == tile_y) {
			return stored_slot - 1;
		}
	}
	return -1;
}

vec2 height_vertex(int level, int face, int ix, int iy, int cells) {
	int tile_cells = int(u_page_tile_cells + 0.5);
	int tile_verts = int(u_page_tile_verts + 0.5);
	int tile_count = (cells + tile_cells - 1) / tile_cells;
	int tile_x = clamp((max(ix, 1) - 1) / tile_cells, 0, tile_count - 1);
	int tile_y = clamp((max(iy, 1) - 1) / tile_cells, 0, tile_count - 1);
	int local_x = ix - tile_x * tile_cells;
	int local_y = iy - tile_y * tile_cells;
	int slot = page_slot(level, face, tile_x, tile_y);
	if (slot < 0) {
		return vec2(0.0, 0.0);
	}
	int atlas_cols = int(u_page_atlas_cols + 0.5);
	int page_x = slot % atlas_cols;
	int page_y = slot / atlas_cols;
	ivec2 pixel = ivec2(page_x * tile_verts + local_x, page_y * tile_verts + local_y);
	return vec2(texelFetch(u_height_page_atlas, pixel, 0).r, 1.0);
}

vec2 sample_exact_level(vec3 d, int level) {
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

	vec2 h00 = height_vertex(level, face, x0, y0, cells);
	vec2 h10 = height_vertex(level, face, x1, y0, cells);
	vec2 h01 = height_vertex(level, face, x0, y1, cells);
	vec2 h11 = height_vertex(level, face, x1, y1, cells);
	if (min(min(h00.y, h10.y), min(h01.y, h11.y)) < 0.5) {
		return vec2(0.0, 0.0);
	}
	return vec2(mix(mix(h00.x, h10.x, tx), mix(h01.x, h11.x, tx), ty), 1.0);
}

vec2 orbit_fallback(vec3 d) {
	if (u_orbit_ready < 0.5 || u_orbit_face_res <= 0.5) {
		return vec2(0.0, 0.0);
	}
	return vec2(surface_visual_height(u_orbit_elevation, d, u_orbit_face_res, u_planet_radius), 1.0);
}

vec2 sample_best_level(vec3 d, int requested_level) {
	if (u_page_atlas_ready > 0.5) {
		for (int step = 0; step < 15; step++) {
			int candidate = requested_level + step;
			if (candidate > ASTERRA_MAX_LEVEL) {
				break;
			}
			vec2 result = sample_exact_level(d, candidate);
			if (result.y > 0.5) {
				return result;
			}
		}
	}
	return orbit_fallback(d);
}

float fine_morph_weight(vec2 cell) {
	float half_cells = u_grid_cells * 0.5;
	float q = max(abs(cell.x), abs(cell.y)) / max(half_cells, 1.0);
	return smoothstep(FINE_MORPH_START_Q, 1.0, q);
}

float coarse_sink_weight(vec2 cell, int level) {
	if (level <= 0) {
		return 0.0;
	}
	float half_cells = u_grid_cells * 0.5;
	float q = max(abs(cell.x), abs(cell.y)) / max(half_cells, 1.0);
	return 1.0 - smoothstep(RING_INNER_Q, RING_HANDOFF_Q, q);
}

vec2 clip_height(vec3 d, vec2 cell, int level) {
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
	vec3 d = direction_for_offset(offset_m);
	// Blank terrain owns no height texture. Do not even enter the sparse/orbit
	// sampling path when generated height is disabled; the analytic sphere is a
	// valid surface by itself and shader-authored displacement can build from it.
	vec2 h = u_height_enabled > 0.5 ? clip_height(d, cell, level) : vec2(0.0, 1.0);
	float sink = coarse_sink_weight(cell, level) * max(spacing * u_sink_scale, 0.5);
	float height = h.x - sink;
	vec3 p = d * (u_planet_radius + height + u_surface_bias);

	v_planet = p;
	v_render = p - u_origin;
	v_valid = h.y;
	v_cap_valid = raw_theta <= u_visible_cap_angle ? 1.0 : 0.0;
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
	vec4 surf = material_clipmap_surface(v_planet, u_planet_radius,
		vec4(0.38, 0.38, 0.12, 0.0));

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
