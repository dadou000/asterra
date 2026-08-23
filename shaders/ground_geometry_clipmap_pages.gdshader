shader_type spatial;
render_mode cull_disabled, diffuse_burley, specular_schlick_ggx;

#include "res://shaders/material_clipmap.gdshaderinc"

// Persistent sparse GPU height cache. The page-table texture stores
// (level*6+face+1, tile_x, tile_y, atlas_slot+1) in RGBA32F.
uniform sampler2D u_height_page_atlas : filter_nearest, repeat_disable;
uniform sampler2D u_height_page_table : filter_nearest, repeat_disable;
uniform float u_page_atlas_ready = 0.0;
uniform vec2 u_page_atlas_size = vec2(528.0, 528.0);
uniform float u_page_table_capacity = 2048.0;
uniform float u_page_atlas_cols = 16.0;
uniform float u_page_tile_cells = 32.0;
uniform float u_page_tile_verts = 33.0;
uniform float u_finest_cells = 2097152.0;

uniform float u_planet_radius = 1000000.0;
uniform vec3 u_origin = vec3(0.0);
uniform vec3 u_frame_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_frame_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_frame_up = vec3(0.0, 1.0, 0.0);
uniform vec2 u_center_plane = vec2(0.0);
uniform float u_base_spacing = 0.75;
uniform float u_grid_cells = 64.0;
uniform float u_surface_bias = 0.035;

varying vec3 v_planet;
varying vec3 v_world_normal;
varying float v_edge_fade;
varying float v_page_valid;

const float ASTERRA_Q = 0.7853981633974483;
const int ASTERRA_MAX_LEVEL = 6;
const int ASTERRA_TABLE_PROBES = 16;

float ring_spacing(float level) {
	return u_base_spacing * exp2(level);
}

vec3 direction_for_plane(vec2 plane) {
	return normalize(u_frame_dir
		+ u_frame_right * (plane.x / u_planet_radius)
		+ u_frame_up * (plane.y / u_planet_radius));
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
	int page_code = level * 6 + face + 1;
	int start = page_hash(page_code, tile_x, tile_y);
	for (int probe = 0; probe < ASTERRA_TABLE_PROBES; probe++) {
		int table_index = (start + probe) % capacity;
		vec2 uv = vec2((float(table_index) + 0.5) / float(capacity), 0.5);
		vec4 meta = textureLod(u_height_page_table, uv, 0.0);
		int stored_slot = int(floor(meta.a + 0.5));
		if (stored_slot <= 0) {
			return -1;
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
	vec2 pixel = vec2(float(page_x * tile_verts + local_x),
		float(page_y * tile_verts + local_y));
	vec2 uv = (pixel + vec2(0.5)) / u_page_atlas_size;
	return vec2(textureLod(u_height_page_atlas, uv, 0.0).r, 1.0);
}

vec2 sample_exact_level(vec2 plane, int level) {
	vec3 d = direction_for_plane(plane);
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

vec2 sample_best_level(vec2 plane, int requested_level) {
	for (int step = 0; step < 7; step++) {
		int candidate = requested_level + step;
		if (candidate > ASTERRA_MAX_LEVEL) {
			break;
		}
		vec2 result = sample_exact_level(plane, candidate);
		if (result.y > 0.5) {
			return result;
		}
	}
	return vec2(0.0, 0.0);
}

float transition_weight(vec2 cell) {
	float half_cells = u_grid_cells * 0.5;
	float q = max(abs(cell.x), abs(cell.y)) / max(half_cells, 1.0);
	return smoothstep(0.72, 0.90, q);
}

vec2 clip_height(vec2 plane, vec2 cell, int level) {
	vec2 fine = sample_best_level(plane, level);
	if (fine.y < 0.5) {
		return fine;
	}
	if (level < ASTERRA_MAX_LEVEL) {
		vec2 coarse = sample_best_level(plane, level + 1);
		if (coarse.y > 0.5) {
			fine.x = mix(fine.x, coarse.x, transition_weight(cell));
		}
	}
	return fine;
}

vec4 planet_position(vec2 cell, int level) {
	float spacing = ring_spacing(float(level));
	vec2 plane = u_center_plane + cell * spacing;
	vec3 d = direction_for_plane(plane);
	vec2 h = clip_height(plane, cell, level);
	return vec4(d * (u_planet_radius + h.x + u_surface_bias), h.y);
}

void vertex() {
	int grid_verts = int(u_grid_cells + 1.5);
	int grid_x = VERTEX_ID % grid_verts;
	int grid_y = VERTEX_ID / grid_verts;
	vec2 cell = vec2(float(grid_x), float(grid_y)) - vec2(u_grid_cells * 0.5);
	int clip_level = int(floor(INSTANCE_CUSTOM.x + 0.5));
	float clip_outer = INSTANCE_CUSTOM.y;

	if (u_page_atlas_ready < 0.5) {
		VERTEX = vec3(0.0);
		NORMAL = vec3(0.0, 1.0, 0.0);
		v_planet = vec3(u_planet_radius, 0.0, 0.0);
		v_world_normal = normalize(v_planet);
		v_edge_fade = 1.0;
		v_page_valid = 0.0;
	} else {
		vec4 p4 = planet_position(cell, clip_level);
		vec4 pl4 = planet_position(cell + vec2(-1.0, 0.0), clip_level);
		vec4 pr4 = planet_position(cell + vec2( 1.0, 0.0), clip_level);
		vec4 pd4 = planet_position(cell + vec2(0.0, -1.0), clip_level);
		vec4 pu4 = planet_position(cell + vec2(0.0,  1.0), clip_level);
		float valid = min(p4.w, min(min(pl4.w, pr4.w), min(pd4.w, pu4.w)));
		vec3 p = p4.xyz;
		vec3 n = normalize(cross(pr4.xyz - pl4.xyz, pu4.xyz - pd4.xyz));
		if (dot(n, normalize(p)) < 0.0) {
			n = -n;
		}

		v_planet = p;
		v_world_normal = n;
		v_page_valid = valid;
		NORMAL = n;
		VERTEX = p - u_origin;

		float q = max(abs(cell.x), abs(cell.y)) / max(u_grid_cells * 0.5, 1.0);
		v_edge_fade = clip_outer > 0.5 ? smoothstep(0.945, 0.995, q) : 0.0;
	}
}

float dither_noise(vec2 p) {
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void fragment() {
	if (v_page_valid < 0.5) {
		discard;
	}
	if (v_edge_fade > 0.001 && dither_noise(FRAGCOORD.xy) < v_edge_fade) {
		discard;
	}

	vec3 up = normalize(v_planet);
	vec3 wn = normalize(v_world_normal);
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
