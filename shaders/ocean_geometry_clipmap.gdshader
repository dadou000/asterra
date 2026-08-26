shader_type spatial;
render_mode blend_mix, cull_disabled, depth_draw_always, diffuse_burley, specular_schlick_ggx, ambient_light_disabled;

#include "res://shaders/atmosphere.gdshaderinc"
#include "res://shaders/cloud_shadow.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/gpu_terrain_height.gdshaderinc"
#include "res://shaders/water_surface.gdshaderinc"
#include "res://shaders/ocean_waves.gdshaderinc"

uniform vec3 u_origin = vec3(0.0);
uniform vec3 u_center_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_center_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_center_up = vec3(0.0, 1.0, 0.0);
uniform vec3 u_anchor_render = vec3(0.0);
uniform vec3 u_anchor_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_anchor_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_anchor_up = vec3(0.0, 1.0, 0.0);
uniform vec2 u_lattice_center_plane = vec2(0.0);
uniform float u_grid_cells = 128.0;
uniform float u_visible_cap_angle = 1.5707963;
uniform float u_orbit_handoff_altitude = 120000.0;
uniform float u_wave_scale : hint_range(0.0, 1.0) = 1.0;
uniform float u_stable_displacement = 1.0;
uniform vec3 u_deep : source_color = vec3(0.010, 0.042, 0.105);
uniform vec3 u_shelf : source_color = vec3(0.058, 0.300, 0.360);
uniform vec3 u_shallow : source_color = vec3(0.130, 0.475, 0.440);
uniform vec3 u_foam : source_color = vec3(0.82, 0.90, 0.94);

varying vec3 v_planet;
varying vec3 v_render;
varying vec3 v_slope;
varying float v_depth;
varying float v_valid;
varying float v_level;
varying float v_cloud_shadow;
varying float v_crest;
varying float v_breaking;
varying float v_compression;

const float MAX_PROJECTED_THETA = 1.62;
const float TANGENT_APPROX_MAX_ARC_M = 90000.0;
const float TERRAIN_HALF_CELLS = 200.0;
const float TERRAIN_FINE_MORPH_START_Q = 0.875;
const float OCEAN_FINE_MORPH_START_Q = 0.875;
const int OCEAN_MAX_LEVEL = 14;

const vec3 OCEAN_SWELL_AXIS_A = vec3(0.827, 0.201, 0.525);
const vec3 OCEAN_SWELL_AXIS_B = vec3(-0.436, 0.331, 0.837);

float ocean_ring_spacing(int level) {
	return u_base_spacing * exp2(float(level));
}

float ocean_smoother01(float x) {
	x = clamp(x, 0.0, 1.0);
	return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

float ocean_smoother_range(float edge0, float edge1, float x) {
	return ocean_smoother01((x - edge0) / max(edge1 - edge0, 1e-9));
}

vec3 ocean_direction_for_basis_offset(vec3 center, vec3 right, vec3 up,
		vec2 offset_m) {
	float arc = length(offset_m);
	if (arc < 1e-5) {
		return normalize(center);
	}
	if (arc <= TANGENT_APPROX_MAX_ARC_M) {
		return normalize(center
			+ right * (offset_m.x / u_planet_radius)
			+ up * (offset_m.y / u_planet_radius));
	}
	float theta = min(arc / max(u_planet_radius, 1.0),
		min(u_visible_cap_angle * 1.02, MAX_PROJECTED_THETA));
	vec3 tangent = normalize(right * offset_m.x + up * offset_m.y);
	return normalize(center * cos(theta) + tangent * sin(theta));
}

vec3 ocean_direction_for_offset(vec2 offset_m) {
	return ocean_direction_for_basis_offset(u_center_dir, u_center_right,
		u_center_up, offset_m);
}

vec3 ocean_stable_surface_offset(vec3 center, vec3 right, vec3 up,
		vec2 offset_m, float altitude_m) {
	float radius = max(u_planet_radius, 1.0);
	float arc = length(offset_m);
	if (arc < 1e-5) {
		return center * altitude_m;
	}

	if (arc <= TANGENT_APPROX_MAX_ARC_M) {
		vec3 tangent_m = right * offset_m.x + up * offset_m.y;
		float q2 = dot(tangent_m, tangent_m) / (radius * radius);
		float root = sqrt(1.0 + q2);
		float inv_root = 1.0 / root;
		float inv_root_minus_one = -q2 / (root * (root + 1.0));
		float radial_m = radius * inv_root_minus_one + altitude_m * inv_root;
		float tangent_scale = (1.0 + altitude_m / radius) * inv_root;
		return center * radial_m + tangent_m * tangent_scale;
	}

	float theta = min(arc / radius,
		min(u_visible_cap_angle * 1.02, MAX_PROJECTED_THETA));
	vec3 tangent = normalize(right * offset_m.x + up * offset_m.y);
	float sin_theta = sin(theta);
	float cos_theta = cos(theta);
	float half_sin = sin(theta * 0.5);
	float radial_m = -2.0 * radius * half_sin * half_sin
		+ altitude_m * cos_theta;
	float tangent_m = (radius + altitude_m) * sin_theta;
	return center * radial_m + tangent * tangent_m;
}

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

float ocean_visible_ground_height(vec3 d, float arc_m) {
	int terrain_level;
	float terrain_morph;
	ocean_visible_terrain_lod(arc_m, terrain_level, terrain_morph);
	return gpu_terrain_height_with_morph(d, terrain_level, terrain_morph);
}

float ocean_coast_height_fast(vec3 d) {
	return textureLod(u_macro_elevation,
		surface_array_coord(normalize(d), u_macro_face_res), 0.0).r;
}

void ocean_coast_frame(vec3 d, float ground_h, float spacing,
		out vec3 landward, out float signed_shore_distance,
		out float confidence) {
	vec3 tx = ocean_safe_tangent(d, vec3(0.0, 1.0, 0.0));
	vec3 ty = normalize(cross(d, tx));
	float macro_texel_m = (PI * 0.5 * u_planet_radius) / max(u_macro_face_res, 1.0);
	float sample_m = clamp(max(macro_texel_m * 0.72, spacing * 4.0), 64.0, 2400.0);
	float angle = sample_m / max(u_planet_radius, 1.0);
	float ca = cos(angle);
	float sa = sin(angle);

	vec3 dxp = normalize(d * ca + tx * sa);
	vec3 dxm = normalize(d * ca - tx * sa);
	vec3 dyp = normalize(d * ca + ty * sa);
	vec3 dym = normalize(d * ca - ty * sa);

	float gx = (ocean_coast_height_fast(dxp) - ocean_coast_height_fast(dxm))
		/ max(2.0 * sample_m, 1.0);
	float gy = (ocean_coast_height_fast(dyp) - ocean_coast_height_fast(dym))
		/ max(2.0 * sample_m, 1.0);
	vec3 gradient = tx * gx + ty * gy;
	float slope = length(gradient);

	confidence = smoothstep(0.00035, 0.0035, slope);
	vec3 fallback = ocean_safe_tangent(d, OCEAN_SWELL_AXIS_A);
	landward = slope > 1e-6 ? gradient / slope : fallback;

	if (slope > 0.00015) {
		signed_shore_distance = clamp(ground_h / slope, -30000.0, 30000.0);
	} else {
		float sign_h = ground_h >= 0.0 ? 1.0 : -1.0;
		signed_shore_distance = sign_h * min(abs(ground_h) * 8.0, 30000.0);
	}
}

float ocean_effective_wave_spacing(vec2 cell, int level, float spacing) {
	if (level >= OCEAN_MAX_LEVEL) return spacing;
	float half_cells = max(u_grid_cells * 0.5, 1.0);
	float q = length(cell) / half_cells;
	float morph = ocean_smoother_range(OCEAN_FINE_MORPH_START_Q, 1.0, q);
	return mix(spacing, spacing * 2.0, morph);
}

void vertex() {
	int grid_verts = int(u_grid_cells + 1.5);
	int gx = VERTEX_ID % grid_verts;
	int gy = VERTEX_ID / grid_verts;
	vec2 cell = vec2(float(gx), float(gy)) - vec2(u_grid_cells * 0.5);
	int level = int(floor(INSTANCE_CUSTOM.x + 0.5));
	float spacing = ocean_ring_spacing(level);
	float wave_spacing = ocean_effective_wave_spacing(cell, level, spacing);
	vec2 cell_offset_m = cell * spacing;
	float arc_m = length(cell_offset_m);
	float raw_theta = arc_m / max(u_planet_radius, 1.0);
	bool stable_displacement = u_stable_displacement > 0.5;
	vec2 level_center_m = round(u_lattice_center_plane / spacing) * spacing;
	vec2 anchor_offset_m = level_center_m + cell_offset_m;
	vec3 d = stable_displacement
		? ocean_direction_for_basis_offset(u_anchor_dir, u_anchor_right, u_anchor_up,
			anchor_offset_m)
		: ocean_direction_for_offset(cell_offset_m);

	float ground_h = ocean_visible_ground_height(d, arc_m);
	float depth = max(-ground_h, 0.0);

	vec3 landward;
	float shore_distance;
	float coast_confidence;
	ocean_coast_frame(d, ground_h, wave_spacing,
		landward, shore_distance, coast_confidence);

	vec3 swell_a = ocean_safe_tangent(d, OCEAN_SWELL_AXIS_A);
	vec3 swell_b = ocean_safe_tangent(d, OCEAN_SWELL_AXIS_B);

	float refract = (1.0 - smoothstep(22.0, 150.0, depth)) * coast_confidence;
	vec3 refracted_a = ocean_safe_tangent(d, mix(swell_a, landward, refract));
	vec3 refracted_b = ocean_safe_tangent(d, mix(swell_b, landward, refract * 0.88));
	float diffract_a = ocean_diffraction_weight(d, swell_a, landward, depth)
		* coast_confidence;
	float diffract_b = ocean_diffraction_weight(d, swell_b, landward, depth)
		* coast_confidence * 0.82;
	vec3 dir_a = ocean_safe_tangent(d, mix(refracted_a,
		ocean_diffraction_direction(d, swell_a, landward), diffract_a * 0.52));
	vec3 dir_b = ocean_safe_tangent(d, mix(refracted_b,
		ocean_diffraction_direction(d, swell_b, landward), diffract_b * 0.46));

	float phase_a = ocean_geodesic_phase_coord(d, OCEAN_SWELL_AXIS_A);
	float phase_b = ocean_geodesic_phase_coord(d, OCEAN_SWELL_AXIS_B);

	float solver_depth = max(depth, 0.35);
	OceanWaveSample wave = ocean_empty_wave();
	ocean_add_wave(d, dir_a, phase_a, shore_distance, solver_depth, wave_spacing,
		96.0, 0.78 * u_wave_scale, 0.62, TIME, 0.0, wave);
	ocean_add_wave(d, dir_b, phase_b, shore_distance, solver_depth, wave_spacing,
		42.0, 0.32 * u_wave_scale, 0.55, TIME, 1.7, wave);
	ocean_add_wave(d, dir_a, phase_a, shore_distance, solver_depth, wave_spacing,
		18.0, 0.105 * u_wave_scale, 0.48, TIME, 3.1, wave);
	ocean_add_wave(d, dir_b, phase_b, shore_distance, solver_depth, wave_spacing,
		8.0, 0.036 * u_wave_scale, 0.42, TIME, 4.6, wave);

	vec3 base_planet = d * u_planet_radius;
	vec3 displaced = base_planet + wave.displacement;
	float radial_wave_h = dot(wave.displacement, d);
	vec3 horizontal_wave = wave.displacement - d * radial_wave_h;

	v_planet = displaced;
	v_render = stable_displacement
		? u_anchor_render + ocean_stable_surface_offset(u_anchor_dir, u_anchor_right,
			u_anchor_up, anchor_offset_m, radial_wave_h) + horizontal_wave
		: displaced - u_origin;
	v_slope = wave.slope;
	v_depth = depth;
	v_level = float(level);
	v_crest = wave.crest;
	v_breaking = wave.breaking;
	v_compression = wave.compression;

	vec3 shadow_sun = normalize(u_cloud_shadow_sun_dir);
	float planet_vis = planet_direct_sun_visibility(displaced, shadow_sun, u_planet_radius);
	v_cloud_shadow = planet_vis > 0.001
		? cloud_shadow_transmittance(displaced, shadow_sun, u_planet_radius)
		: 0.0;

	v_valid = (u_macro_ready > 0.5 && raw_theta <= u_visible_cap_angle) ? 1.0 : 0.0;
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

	float surf_zone = 1.0 - smoothstep(0.5, 12.0, depth);
	float breaker_foam = v_breaking * mix(0.72, 1.0, v_crest);
	float compression_foam = v_compression * surf_zone * 0.48;
	float wash_foam = (1.0 - smoothstep(0.15, 2.2, depth))
		* smoothstep(0.48, 0.90, v_crest) * 0.62;
	float foam = clamp(max(max(breaker_foam, compression_foam), wash_foam), 0.0, 1.0);
	base = mix(base, u_foam, foam * 0.86);

	vec3 sun_dir = normalize(u_sun_dir);
	vec3 skylight = planet_sky_irradiance(v_planet, sun_dir, u_planet_radius);

	vec3 ps = up * u_planet_radius;
	float gust = surface_vnoise(ps * 0.000032 + vec3(5.0, -17.0, 26.0));
	float rough = water_roughness_from_wind(water_wind_speed(up), gust);
	rough = clamp(rough + clamp(length(v_slope) * 0.18, 0.0, 0.11)
		+ foam * 0.42, 0.025, 0.82);

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
	ALPHA = mix(0.72, 1.0, smoothstep(0.0, 7.0, depth));
	ALPHA = mix(ALPHA, 1.0, foam * 0.75);
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
