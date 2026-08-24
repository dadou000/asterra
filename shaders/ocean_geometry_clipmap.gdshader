shader_type spatial;
render_mode blend_mix, cull_disabled, depth_draw_always, diffuse_burley, specular_schlick_ggx, ambient_light_disabled;

#include "res://shaders/atmosphere.gdshaderinc"
#include "res://shaders/cloud_shadow.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/gpu_terrain_height.gdshaderinc"
#include "res://shaders/water_surface.gdshaderinc"

// TEMPORARY OCEAN LOD DIAGNOSTIC
// --------------------------------
// Every clipmap ring evaluates one and only one world-space sawtooth function.
// There is deliberately no Gerstner spectrum, refraction, diffraction, shoaling,
// depth-dependent frequency, LOD wave filtering, horizontal displacement or
// coarse-ring sink. If two rings address the same planet-space point, they must
// return exactly the same water height and slope.

uniform vec3 u_origin = vec3(0.0);
uniform vec3 u_center_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_center_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_center_up = vec3(0.0, 1.0, 0.0);
uniform float u_grid_cells = 128.0;
uniform float u_visible_cap_angle = 1.5707963;
uniform float u_orbit_handoff_altitude = 120000.0;
uniform vec3 u_deep : source_color = vec3(0.010, 0.042, 0.105);
uniform vec3 u_shelf : source_color = vec3(0.058, 0.300, 0.360);
uniform vec3 u_shallow : source_color = vec3(0.130, 0.475, 0.440);

varying vec3 v_planet;
varying vec3 v_render;
varying vec3 v_slope;
varying float v_depth;
varying float v_valid;
varying float v_saw_phase;
varying float v_level;
varying float v_cloud_shadow;

const float MAX_PROJECTED_THETA = 1.62;
const float TANGENT_APPROX_MAX_ARC_M = 90000.0;
const float TERRAIN_HALF_CELLS = 200.0;
const float TERRAIN_FINE_MORPH_START_Q = 0.875;

// Long enough to remain readable on the coarse LODs visible from ~1 km altitude.
// A 24 s period makes phase/frequency disagreement obvious without moving too fast.
const float DEBUG_SAW_WAVELENGTH_M = 8192.0;
const float DEBUG_SAW_AMPLITUDE_M = 36.0;
const float DEBUG_SAW_FREQUENCY_HZ = 1.0 / 24.0;
const vec3 DEBUG_SAW_AXIS = vec3(0.827, 0.201, 0.525);

float ocean_ring_spacing(int level) {
	return u_base_spacing * exp2(float(level));
}

float ocean_smoother01(float x) {
	x = clamp(x, 0.0, 1.0);
	return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

vec3 ocean_safe_tangent(vec3 d, vec3 candidate) {
	vec3 t = candidate - d * dot(candidate, d);
	float l2 = dot(t, t);
	if (l2 < 1e-8) {
		vec3 axis = abs(d.y) < 0.85 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		t = cross(axis, d);
		l2 = max(dot(t, t), 1e-8);
	}
	return t * inversesqrt(l2);
}

vec3 ocean_direction_for_offset(vec2 offset_m) {
	float arc = length(offset_m);
	if (arc < 1e-5) {
		return normalize(u_center_dir);
	}
	if (arc <= TANGENT_APPROX_MAX_ARC_M) {
		return normalize(u_center_dir
			+ u_center_right * (offset_m.x / u_planet_radius)
			+ u_center_up * (offset_m.y / u_planet_radius));
	}
	float theta = min(arc / max(u_planet_radius, 1.0),
		min(u_visible_cap_angle * 1.02, MAX_PROJECTED_THETA));
	vec3 tangent = normalize(u_center_right * offset_m.x + u_center_up * offset_m.y);
	return normalize(u_center_dir * cos(theta) + tangent * sin(theta));
}

// Signed constant-metric geodesic coordinate. No ring/LOD/clipmap coordinates
// enter this function.
float ocean_geodesic_phase_coord(vec3 d, vec3 axis) {
	float mu = clamp(dot(normalize(d), normalize(axis)), -0.999999, 0.999999);
	return u_planet_radius * asin(mu);
}

void ocean_visible_terrain_lod(float arc_m, out int terrain_level, out float terrain_morph) {
	float l0_outer = max(u_base_spacing * TERRAIN_HALF_CELLS, 1.0);
	float ratio = max(arc_m / l0_outer, 1.0);
	terrain_level = clamp(int(ceil(log2(ratio))), 0, GPU_TERRAIN_MAX_LEVEL);
	float outer = l0_outer * exp2(float(terrain_level));
	float q = arc_m / max(outer, 1.0);
	terrain_morph = terrain_level < GPU_TERRAIN_MAX_LEVEL
		? gpu_terrain_smootherstep_range(TERRAIN_FINE_MORPH_START_Q, 1.0, q)
		: 0.0;
}

// The terrain is used only as a wet/dry mask and for water colour depth. It does
// not affect diagnostic wave phase, direction, wavelength, amplitude or speed.
float ocean_visible_ground_height(vec3 d, float arc_m) {
	int terrain_level;
	float terrain_morph;
	ocean_visible_terrain_lod(arc_m, terrain_level, terrain_morph);
	return gpu_terrain_height_with_morph(d, terrain_level, terrain_morph);
}

void vertex() {
	int grid_verts = int(u_grid_cells + 1.5);
	int gx = VERTEX_ID % grid_verts;
	int gy = VERTEX_ID / grid_verts;
	vec2 cell = vec2(float(gx), float(gy)) - vec2(u_grid_cells * 0.5);
	int level = int(floor(INSTANCE_CUSTOM.x + 0.5));
	float spacing = ocean_ring_spacing(level);
	vec2 offset_m = cell * spacing;
	float arc_m = length(offset_m);
	float raw_theta = arc_m / max(u_planet_radius, 1.0);
	vec3 d = ocean_direction_for_offset(offset_m);

	float ground_h = ocean_visible_ground_height(d, arc_m);
	float depth = max(-ground_h, 0.0);

	// One global scalar phase field for every ring.
	float phase_m = ocean_geodesic_phase_coord(d, DEBUG_SAW_AXIS);
	float cycles = phase_m / DEBUG_SAW_WAVELENGTH_M - TIME * DEBUG_SAW_FREQUENCY_HZ;
	float saw01 = fract(cycles);
	float saw_height = (saw01 * 2.0 - 1.0) * DEBUG_SAW_AMPLITUDE_M;

	// Between resets, dh/ds is constant. The discontinuity itself is intentionally
	// sharp so a phase mismatch between adjacent rings is immediately visible.
	vec3 travel = ocean_safe_tangent(d, DEBUG_SAW_AXIS);
	float dh_ds = (2.0 * DEBUG_SAW_AMPLITUDE_M) / DEBUG_SAW_WAVELENGTH_M;
	vec3 slope = travel * dh_ds;

	// NO LOD SINK in diagnostic mode. Ring number cannot alter water altitude.
	vec3 displaced = d * (u_planet_radius + saw_height);
	v_planet = displaced;
	v_render = displaced - u_origin;
	v_slope = slope;
	v_depth = depth;
	v_saw_phase = saw01;
	v_level = float(level);

	vec3 shadow_sun = normalize(u_cloud_shadow_sun_dir);
	float planet_vis = planet_direct_sun_visibility(displaced, shadow_sun, u_planet_radius);
	v_cloud_shadow = planet_vis > 0.001
		? cloud_shadow_transmittance(displaced, shadow_sun, u_planet_radius)
		: 0.0;

	v_valid = (u_macro_ready > 0.5 && ground_h < 0.45
		&& raw_theta <= u_visible_cap_angle) ? 1.0 : 0.0;
	VERTEX = v_render;
}

void fragment() {
	if (v_valid < 0.5) {
		discard;
	}

	vec3 camera_planet = CAMERA_POSITION_WORLD + u_origin;
	float camera_alt = length(camera_planet) - u_planet_radius;
	if (camera_alt >= u_orbit_handoff_altitude) {
		discard;
	}

	vec3 up = normalize(v_planet);
	vec3 n = normalize(up - v_slope);
	if (dot(n, up) < 0.0) {
		n = -n;
	}

	float depth = v_depth;
	vec3 base = water_depth_tint(depth, u_shallow, u_shelf, u_deep);

	// Mild phase brightness ramp makes the saw reset line easy to track across
	// rings without assigning each LOD a different debug colour.
	base *= mix(0.72, 1.18, v_saw_phase);

	vec3 sun_dir = normalize(u_sun_dir);
	vec3 skylight = planet_sky_irradiance(v_planet, sun_dir, u_planet_radius);
	float rough = 0.16;

	vec3 view_dir = normalize(camera_planet - v_planet);
	float ndotv = max(dot(n, view_dir), 0.0);
	float fres = water_fresnel(ndotv);
	vec3 sky_refl = water_sky_radiance(reflect(-view_dir, n), up, sun_dir);

	vec3 atmo_t;
	vec3 atmo_l;
	aerial_perspective(camera_planet, v_planet, atmo_t, atmo_l);
	ALBEDO = base * (1.0 - fres * 0.85) * atmo_t;
	EMISSION = (base * skylight + sky_refl * fres * 0.9) * atmo_t + atmo_l;
	NORMAL = normalize((VIEW_MATRIX * vec4(n, 0.0)).xyz);
	ROUGHNESS = rough;
	SPECULAR = 0.5;
	METALLIC = 0.0;
	AO = 1.0;
	AO_LIGHT_AFFECT = 1.0;
	ALPHA = mix(0.76, 1.0, smoothstep(0.0, 6.0, depth));
}

void light() {
	float visibility = 1.0;
	if (LIGHT_IS_DIRECTIONAL) {
		visibility = planet_direct_sun_visibility(v_planet, normalize(u_sun_dir), u_planet_radius);
		visibility *= clamp(v_cloud_shadow, 0.0, 1.0);
	}
	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	DIFFUSE_LIGHT += ndotl * ATTENUATION * LIGHT_COLOR * visibility / PI;
	float spec = water_specular(NORMAL, VIEW, LIGHT, ROUGHNESS);
	float fres = water_fresnel(max(dot(NORMAL, VIEW), 0.0));
	SPECULAR_LIGHT += LIGHT_COLOR * ATTENUATION * visibility * spec * fres * SPECULAR_AMOUNT;
}
