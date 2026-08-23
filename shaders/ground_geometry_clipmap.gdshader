shader_type spatial;
render_mode cull_disabled, diffuse_burley, specular_schlick_ggx;

#include "res://shaders/material_clipmap.gdshaderinc"

// Each clipmap level owns a persistent DrawableTexture2D. Keeping the levels as
// separate textures lets the CPU/GPU streamer blit only newly exposed toroidal
// rows/columns instead of replacing a complete Texture2DArray layer. Filtering is
// manual so sampling remains correct across each circular storage origin.
uniform sampler2D u_height0 : filter_nearest, repeat_disable;
uniform sampler2D u_height1 : filter_nearest, repeat_disable;
uniform sampler2D u_height2 : filter_nearest, repeat_disable;
uniform sampler2D u_height3 : filter_nearest, repeat_disable;
uniform sampler2D u_height4 : filter_nearest, repeat_disable;
uniform sampler2D u_height5 : filter_nearest, repeat_disable;
uniform sampler2D u_height6 : filter_nearest, repeat_disable;
uniform float u_height_ready = 0.0;
uniform float u_planet_radius = 1000000.0;
uniform vec3 u_origin = vec3(0.0);
uniform vec3 u_frame_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_frame_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_frame_up = vec3(0.0, 1.0, 0.0);
uniform vec2 u_center_plane = vec2(0.0);
uniform float u_base_spacing = 0.75;
uniform float u_grid_cells = 64.0;
uniform float u_height_tex_size = 69.0;
uniform float u_height_half = 34.0;
uniform float u_storage_levels = 7.0;
uniform float u_surface_bias = 0.035;

// xy = physical tangent-plane centre of this storage level.
// zw = circular image origin: logical texel (0,0) lives at this storage texel.
uniform vec4 u_height_layout0 = vec4(0.0);
uniform vec4 u_height_layout1 = vec4(0.0);
uniform vec4 u_height_layout2 = vec4(0.0);
uniform vec4 u_height_layout3 = vec4(0.0);
uniform vec4 u_height_layout4 = vec4(0.0);
uniform vec4 u_height_layout5 = vec4(0.0);
uniform vec4 u_height_layout6 = vec4(0.0);

instance uniform float clip_level = 0.0;
instance uniform float clip_outer = 0.0;

varying vec3 v_planet;
varying vec3 v_world_normal;
varying vec2 v_cell;
varying float v_edge_fade;

float ring_spacing(float level) {
	return u_base_spacing * exp2(level);
}

vec4 height_layout(int level) {
	if (level <= 0) { return u_height_layout0; }
	if (level == 1) { return u_height_layout1; }
	if (level == 2) { return u_height_layout2; }
	if (level == 3) { return u_height_layout3; }
	if (level == 4) { return u_height_layout4; }
	if (level == 5) { return u_height_layout5; }
	return u_height_layout6;
}

vec2 wrap_texel(vec2 p) {
	return mod(mod(p, vec2(u_height_tex_size)) + vec2(u_height_tex_size),
		vec2(u_height_tex_size));
}

float height_texel(vec2 uv, int level) {
	if (level <= 0) { return textureLod(u_height0, uv, 0.0).r; }
	if (level == 1) { return textureLod(u_height1, uv, 0.0).r; }
	if (level == 2) { return textureLod(u_height2, uv, 0.0).r; }
	if (level == 3) { return textureLod(u_height3, uv, 0.0).r; }
	if (level == 4) { return textureLod(u_height4, uv, 0.0).r; }
	if (level == 5) { return textureLod(u_height5, uv, 0.0).r; }
	return textureLod(u_height6, uv, 0.0).r;
}

float stored_height(ivec2 logical_texel, int level, vec4 layout) {
	vec2 storage = wrap_texel(vec2(logical_texel) + layout.zw);
	vec2 uv = (storage + vec2(0.5)) / u_height_tex_size;
	return height_texel(uv, level);
}

float sample_level_plane(vec2 plane, float level_f) {
	int level = int(floor(level_f + 0.5));
	vec4 layout = height_layout(level);
	float spacing = ring_spacing(float(level));
	vec2 logical = (plane - layout.xy) / spacing + vec2(u_height_half);
	vec2 base = floor(logical);
	vec2 f = fract(logical);
	ivec2 p00 = ivec2(base);
	ivec2 p10 = p00 + ivec2(1, 0);
	ivec2 p01 = p00 + ivec2(0, 1);
	ivec2 p11 = p00 + ivec2(1, 1);
	float h00 = stored_height(p00, level, layout);
	float h10 = stored_height(p10, level, layout);
	float h01 = stored_height(p01, level, layout);
	float h11 = stored_height(p11, level, layout);
	return mix(mix(h00, h10, f.x), mix(h01, h11, f.x), f.y);
}

float transition_weight(vec2 cell) {
	float half_cells = u_grid_cells * 0.5;
	float q = max(abs(cell.x), abs(cell.y)) / max(half_cells, 1.0);
	return smoothstep(0.72, 0.90, q);
}

float clip_height(vec2 plane, vec2 cell, float level) {
	float h = sample_level_plane(plane, level);
	if (level + 0.5 < u_storage_levels - 1.0) {
		float hc = sample_level_plane(plane, level + 1.0);
		h = mix(h, hc, transition_weight(cell));
	}
	return h;
}

vec3 direction_for_plane(vec2 plane) {
	return normalize(u_frame_dir
		+ u_frame_right * (plane.x / u_planet_radius)
		+ u_frame_up * (plane.y / u_planet_radius));
}

vec3 planet_position(vec2 cell, float level) {
	float spacing = ring_spacing(level);
	vec2 plane = u_center_plane + cell * spacing;
	vec3 d = direction_for_plane(plane);
	float h = clip_height(plane, cell, level);
	return d * (u_planet_radius + h + u_surface_bias);
}

void vertex() {
	// Vercidium-style grid reconstruction: the index stream chooses only a logical
	// vertex ID. No position, UV or normal attribute is needed for terrain data.
	int grid_verts = int(u_grid_cells + 1.5);
	int grid_x = VERTEX_ID % grid_verts;
	int grid_y = VERTEX_ID / grid_verts;
	vec2 cell = vec2(float(grid_x), float(grid_y)) - vec2(u_grid_cells * 0.5);
	v_cell = cell;

	if (u_height_ready < 0.5) {
		VERTEX = vec3(0.0);
		NORMAL = vec3(0.0, 1.0, 0.0);
		v_planet = vec3(u_planet_radius, 0.0, 0.0);
		v_world_normal = normalize(v_planet);
		v_edge_fade = 1.0;
	} else {
		vec3 p = planet_position(cell, clip_level);
		vec3 pl = planet_position(cell + vec2(-1.0, 0.0), clip_level);
		vec3 pr = planet_position(cell + vec2( 1.0, 0.0), clip_level);
		vec3 pd = planet_position(cell + vec2(0.0, -1.0), clip_level);
		vec3 pu = planet_position(cell + vec2(0.0,  1.0), clip_level);
		vec3 n = normalize(cross(pr - pl, pu - pd));
		if (dot(n, normalize(p)) < 0.0) {
			n = -n;
		}

		v_planet = p;
		v_world_normal = n;
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
