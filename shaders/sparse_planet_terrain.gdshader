shader_type spatial;
render_mode cull_disabled, diffuse_burley, specular_schlick_ggx;

#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/material_clipmap.gdshaderinc"

uniform sampler2DArray u_orbit_elevation;
uniform float u_orbit_face_res = 192.0;
uniform float u_orbit_ready = 0.0;
uniform float u_planet_radius = 1000000.0;
uniform float u_atmosphere_height = 60000.0;
uniform float u_global_grid_cells = 192.0;
uniform vec3 u_origin = vec3(0.0);
uniform vec3 u_sun_dir = vec3(1.0, 0.15, 0.3);
uniform float u_sun_intensity = 1.0;
uniform float u_relief_ready = 0.0;

// Compatibility/debug uniforms used by TerrainDebug and the local clipmap handoff.
uniform float u_debug_uv : hint_range(0.0, 1.0) = 0.0;
uniform float u_debug_lod : hint_range(0.0, 1.0) = 0.0;
uniform float u_debug_plain : hint_range(0.0, 1.0) = 0.0;
uniform float u_ground_clipmap_cutout = 0.0;
uniform vec3 u_ground_clipmap_frame_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_ground_clipmap_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_ground_clipmap_up = vec3(0.0, 1.0, 0.0);
uniform vec2 u_ground_clipmap_center_plane = vec2(0.0);
uniform float u_ground_clipmap_cut_half_extent = 0.0;

varying vec3 v_planet;
varying float v_height;
varying float v_face;

const float ASTERRA_Q = 0.7853981633974483;

vec3 face_dir(int face, float u, float v) {
	float tu = tan(u * ASTERRA_Q);
	float tv = tan(v * ASTERRA_Q);
	vec3 axis;
	vec3 right;
	vec3 up;
	if (face == 0) {
		axis = vec3(1.0, 0.0, 0.0); right = vec3(0.0, 0.0, -1.0); up = vec3(0.0, 1.0, 0.0);
	} else if (face == 1) {
		axis = vec3(-1.0, 0.0, 0.0); right = vec3(0.0, 0.0, 1.0); up = vec3(0.0, 1.0, 0.0);
	} else if (face == 2) {
		axis = vec3(0.0, 1.0, 0.0); right = vec3(-1.0, 0.0, 0.0); up = vec3(0.0, 0.0, 1.0);
	} else if (face == 3) {
		axis = vec3(0.0, -1.0, 0.0); right = vec3(-1.0, 0.0, 0.0); up = vec3(0.0, 0.0, -1.0);
	} else if (face == 4) {
		axis = vec3(0.0, 0.0, 1.0); right = vec3(1.0, 0.0, 0.0); up = vec3(0.0, 1.0, 0.0);
	} else {
		axis = vec3(0.0, 0.0, -1.0); right = vec3(-1.0, 0.0, 0.0); up = vec3(0.0, 1.0, 0.0);
	}
	return normalize(axis + right * tu + up * tv);
}

void vertex() {
	int grid_verts = int(u_global_grid_cells + 1.5);
	int gx = VERTEX_ID % grid_verts;
	int gy = VERTEX_ID / grid_verts;
	float u = -1.0 + 2.0 * float(gx) / max(u_global_grid_cells, 1.0);
	float v = -1.0 + 2.0 * float(gy) / max(u_global_grid_cells, 1.0);
	int face = int(floor(INSTANCE_CUSTOM.r + 0.5));
	vec3 d = face_dir(face, u, v);
	float h = 0.0;
	if (u_orbit_ready > 0.5) {
		h = surface_visual_height(u_orbit_elevation, d, u_orbit_face_res, u_planet_radius);
	}
	vec3 p = d * (u_planet_radius + h);
	v_planet = p;
	v_height = h;
	v_face = float(face);
	VERTEX = p - u_origin;
	NORMAL = d;
}

void fragment() {
	vec3 up = normalize(v_planet);

	// The high-resolution local sparse clipmap owns this region. Keeping the cut
	// in this GPU backing surface preserves the existing seamless handoff without
	// requiring any CPU quadtree geometry underneath it.
	if (u_ground_clipmap_cutout > 0.5) {
		vec3 frame_dir = normalize(u_ground_clipmap_frame_dir);
		float denom = dot(up, frame_dir);
		if (denom > 0.08) {
			vec2 plane = vec2(dot(up, normalize(u_ground_clipmap_right)),
				dot(up, normalize(u_ground_clipmap_up))) / denom * u_planet_radius;
			vec2 local_plane = plane - u_ground_clipmap_center_plane;
			if (max(abs(local_plane.x), abs(local_plane.y)) < u_ground_clipmap_cut_half_extent) {
				discard;
			}
		}
	}

	// Derivative normal gives the displaced orbital surface a geometric lighting
	// normal without four extra elevation texture samples per vertex.
	vec3 dx = dFdx(v_planet);
	vec3 dy = dFdy(v_planet);
	vec3 wn = normalize(cross(dx, dy));
	if (dot(wn, up) < 0.0) {
		wn = -wn;
	}
	NORMAL = normalize((VIEW_MATRIX * vec4(wn, 0.0)).xyz);

	float slope = clamp(1.0 - dot(wn, up), 0.0, 1.0);
	vec4 surf = material_clipmap_surface(v_planet, u_planet_radius,
		vec4(0.38, 0.32, 0.12, 0.0));
	vec3 soil = vec3(0.25, 0.18, 0.105);
	vec3 grass = vec3(0.18, 0.27, 0.10);
	vec3 wet = vec3(0.11, 0.15, 0.09);
	vec3 sand = vec3(0.48, 0.40, 0.24);
	vec3 rock = vec3(0.30, 0.29, 0.27);
	vec3 snow = vec3(0.79, 0.81, 0.82);

	vec3 albedo = mix(soil, grass, clamp(surf.r, 0.0, 1.0));
	albedo = mix(albedo, wet, clamp(surf.g * 0.7, 0.0, 1.0));
	albedo = mix(albedo, sand, clamp(surf.b, 0.0, 1.0));
	albedo = mix(albedo, snow, clamp(surf.a, 0.0, 1.0));
	albedo = mix(albedo, rock, smoothstep(0.08, 0.32, slope));

	if (u_debug_plain > 0.5) {
		albedo = vec3(0.42);
	}
	if (u_debug_lod > 0.5) {
		albedo = 0.5 + 0.5 * cos(6.2831853 * (v_face * 0.13 + vec3(0.0, 0.33, 0.67)));
	}

	ALBEDO = albedo;
	ROUGHNESS = mix(0.94, 0.72, smoothstep(0.10, 0.42, slope));
	SPECULAR = 0.20;
}
