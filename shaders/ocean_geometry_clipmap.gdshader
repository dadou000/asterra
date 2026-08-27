shader_type spatial;
render_mode blend_mix, cull_disabled, depth_draw_always, diffuse_burley, specular_schlick_ggx, ambient_light_disabled;

#include "res://shaders/atmosphere.gdshaderinc"
#include "res://shaders/cloud_shadow.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/gpu_terrain_height.gdshaderinc"
#include "res://shaders/water_surface.gdshaderinc"
#include "res://shaders/ocean_waves.gdshaderinc"

// Production ocean representation.
// Large/medium wavelengths displace the clipmap. Once a wavelength is no longer
// resolvable by the current mesh or graphics preset, its slope energy is retained
// as microfacet variance rather than disappearing. This is what prevents distant
// rough water from becoming a polished mirror.

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
uniform float u_time_s = 0.0;

uniform int u_quality_band_count = 5;
uniform int u_micro_band_count = 4;
uniform float u_displacement_spacing_scale = 1.0;
uniform int u_bathymetry_level_limit = 7;
uniform float u_foam_quality : hint_range(0.0, 1.0) = 0.9;
uniform float u_crest_scatter_strength : hint_range(0.0, 1.0) = 0.78;

uniform sampler2D u_interaction_tex : filter_nearest, repeat_disable;
uniform int u_interaction_count = 0;
uniform float u_interaction_range_m = 2000.0;
uniform int u_interaction_vertex_level = 4;

uniform vec3 u_deep : source_color = vec3(0.010, 0.042, 0.105);
uniform vec3 u_shelf : source_color = vec3(0.058, 0.300, 0.360);
uniform vec3 u_shallow : source_color = vec3(0.130, 0.475, 0.440);
uniform vec3 u_foam : source_color = vec3(0.88, 0.93, 0.96);
uniform vec3 u_scatter : source_color = vec3(0.055, 0.34, 0.30);

varying vec3 v_planet;
varying vec3 v_render;
varying vec3 v_slope;
varying vec2 v_micro_plane;
varying float v_depth;
varying float v_valid;
varying float v_breaking;
varying float v_crest;
varying float v_compression;
varying float v_diffraction;
varying float v_unresolved_variance;
varying float v_interaction_foam;
varying float v_cloud_shadow;
varying float v_level;

const float MAX_PROJECTED_THETA = 1.62;
const float TANGENT_APPROX_MAX_ARC_M = 90000.0;
const float TERRAIN_HALF_CELLS = 200.0;
const float TERRAIN_FINE_MORPH_START_Q = 0.875;
const float OCEAN_FINE_MORPH_START_Q = 0.875;
const float OCEAN_RING_INNER_Q = 0.4375;
const float OCEAN_RING_HANDOFF_Q = 0.50;

// Six non-parallel physical wave axes avoid the two-direction corrugation that
// made a rough sea read as repeating mountain ridges. The two longest systems
// still own the coastal refraction/diffraction response; the shorter components
// are progressively less coast-steered, as they should be.
const vec3 SWELL_AXIS_A = vec3(0.827, 0.201, 0.525);
const vec3 SWELL_AXIS_B = vec3(-0.436, 0.331, 0.837);
const vec3 SWELL_AXIS_C = vec3(0.153, 0.879, 0.451);
const vec3 SWELL_AXIS_D = vec3(-0.741, 0.529, -0.413);
const vec3 SWELL_AXIS_E = vec3(0.336, -0.612, 0.716);
const vec3 SWELL_AXIS_F = vec3(-0.218, -0.771, -0.598);

const vec3 MICRO_AXIS_A = vec3(0.581, -0.742, 0.334);
const vec3 MICRO_AXIS_B = vec3(-0.703, -0.264, 0.660);
const vec3 MICRO_AXIS_C = vec3(0.246, 0.913, -0.325);
const vec3 MICRO_AXIS_D = vec3(-0.912, 0.128, -0.390);
const vec3 MICRO_AXIS_E = vec3(0.693, 0.451, -0.562);
const vec3 MICRO_AXIS_F = vec3(-0.362, 0.821, 0.441);
const float KELVIN_TAN = 0.35355339;

float ocean_ring_spacing(int level) {
	return u_base_spacing * exp2(float(level));
}

float ocean_smoother01(float x) {
	x = clamp(x, 0.0, 1.0);
	return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

// The fine level gradually adopts the parent's representation bandwidth in its
// outer eighth. At the point where the fine mesh ends, both levels therefore
// represent exactly the same physical wave subset instead of two different seas.
float ocean_fine_morph(vec2 cell) {
	float q = length(cell) / max(u_grid_cells * 0.5, 1.0);
	return ocean_smoother01((q - OCEAN_FINE_MORPH_START_Q)
		/ (1.0 - OCEAN_FINE_MORPH_START_Q));
}

// Adjacent radial clipmap levels overlap from q=0.4375..0.5. The coarse surface
// is lowered only inside that overlap, so depth testing gives ownership to the
// fine layer instead of alpha-blending/z-fighting two displaced surfaces. The
// sink reaches zero exactly at the physical handoff and never changes wave phase.
float ocean_coarse_sink(vec2 cell, int level, float spacing) {
	if (level <= 0) return 0.0;
	float q = length(cell) / max(u_grid_cells * 0.5, 1.0);
	float blend = 1.0 - ocean_smoother01((q - OCEAN_RING_INNER_Q)
		/ (OCEAN_RING_HANDOFF_Q - OCEAN_RING_INNER_Q));
	return blend * clamp(spacing * 0.25, 0.12, 4.0);
}

vec3 ocean_direction_for_basis_offset(vec3 center, vec3 right, vec3 up,
		vec2 offset_m) {
	float arc = length(offset_m);
	if (arc < 1e-5) return normalize(center);
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

vec3 ocean_stable_surface_offset(vec3 center, vec3 right, vec3 up,
		vec2 offset_m, float altitude_m) {
	float radius = max(u_planet_radius, 1.0);
	float arc = length(offset_m);
	if (arc < 1e-5) return center * altitude_m;

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
	float radial_m = -2.0 * radius * half_sin * half_sin + altitude_m * cos_theta;
	float tangent_m = (radius + altitude_m) * sin_theta;
	return center * radial_m + tangent * tangent_m;
}

float ocean_geodesic_phase_coord(vec3 d, vec3 axis) {
	float mu = clamp(dot(normalize(d), normalize(axis)), -0.999999, 0.999999);
	return u_planet_radius * asin(mu);
}

vec3 ocean_offset_dir(vec3 d, vec3 tangent, float metres) {
	float a = metres / max(u_planet_radius, 1.0);
	return normalize(d * cos(a) + tangent * sin(a));
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

void ocean_bathymetry(vec3 d, float arc_m, float sample_m,
		out float raw_ground_h, out float smooth_ground_h,
		out vec3 landward, out float signed_shore_distance,
		out float coast_confidence) {
	raw_ground_h = ocean_visible_ground_height(d, arc_m);
	vec3 tx = ocean_safe_tangent(d, u_center_right);
	vec3 ty = ocean_safe_tangent(d, u_center_up);
	float s = clamp(sample_m, 24.0, 110.0);
	float hx0 = ocean_visible_ground_height(ocean_offset_dir(d, tx, -s), arc_m);
	float hx1 = ocean_visible_ground_height(ocean_offset_dir(d, tx,  s), arc_m);
	float hy0 = ocean_visible_ground_height(ocean_offset_dir(d, ty, -s), arc_m);
	float hy1 = ocean_visible_ground_height(ocean_offset_dir(d, ty,  s), arc_m);
	smooth_ground_h = (raw_ground_h * 4.0 + hx0 + hx1 + hy0 + hy1) * 0.125;
	vec2 g = vec2(hx1 - hx0, hy1 - hy0) / max(2.0 * s, 1.0);
	float gm = length(g);
	coast_confidence = smoothstep(0.025, 0.18, gm);
	landward = gm > 1e-4 ? normalize(tx * g.x + ty * g.y)
		: ocean_safe_tangent(d, SWELL_AXIS_A);
	signed_shore_distance = clamp(smooth_ground_h / max(gm, 0.025), -6000.0, 6000.0);
}

float ocean_band_variance(int band, float wavelength, float base_amp,
		float depth, float spacing) {
	float k = TAU / wavelength;
	float amp = min(base_amp * ocean_shoal_gain(depth, wavelength),
		max(0.015, BREAKER_GAMMA * depth * 0.5));
	float resolved = band < u_quality_band_count
		? ocean_wave_band_weight(wavelength, spacing) : 0.0;
	float slope_rms = amp * k;
	return (1.0 - resolved) * slope_rms * slope_rms * 0.5;
}

void ocean_add_quality_wave(int band, vec3 up, vec3 dir, float phase_coord,
		float shore_distance, float depth, float spacing, float wavelength,
		float amplitude, float steepness, float phase_offset,
		inout OceanWaveSample wave) {
	if (band >= u_quality_band_count) return;
	ocean_add_wave(up, dir, phase_coord, shore_distance, depth, spacing,
		wavelength, amplitude * u_wave_scale, steepness, u_time_s, phase_offset, wave);
}

void ocean_interactions(vec3 d, int level, out float height, out vec3 slope,
		out float foam) {
	height = 0.0;
	slope = vec3(0.0);
	foam = 0.0;
	if (level > u_interaction_vertex_level || u_interaction_count <= 0) return;

	for (int i = 0; i < 64; i++) {
		if (i >= u_interaction_count) break;
		vec4 p0 = texelFetch(u_interaction_tex, ivec2(i, 0), 0);
		vec4 p1 = texelFetch(u_interaction_tex, ivec2(i, 1), 0);
		vec4 p2 = texelFetch(u_interaction_tex, ivec2(i, 2), 0);
		vec4 p3 = texelFetch(u_interaction_tex, ivec2(i, 3), 0);
		vec3 event_dir = normalize(p0.xyz);
		float age = max(u_time_s - p0.w, 0.0);
		vec3 travel = ocean_safe_tangent(event_dir, p1.xyz);
		float event_type = p1.w;
		float amplitude = p2.x * u_wave_scale;
		float radius = max(p2.y, 0.25);
		float wavelength = max(p2.z, 0.25);
		float speed = max(p2.w, 0.1);
		float event_foam = p3.x;

		float angle = acos(clamp(dot(d, event_dir), -1.0, 1.0));
		float dist = angle * u_planet_radius;
		if (dist > u_interaction_range_m) continue;

		vec3 tangent_delta = d - event_dir * dot(d, event_dir);
		vec3 event_to_point = length(tangent_delta) > 1e-7
			? normalize(tangent_delta) : travel;
		float k = TAU / wavelength;

		if (event_type < 0.5) {
			float front = speed * age;
			float signed_front = dist - front;
			float envelope = exp(-abs(signed_front) / max(radius, 0.5))
				* exp(-age * 0.035);
			float phase = signed_front * k;
			height += amplitude * sin(phase) * envelope;
			vec3 radial = ocean_safe_tangent(d, event_dir);
			slope += radial * amplitude * k * cos(phase) * envelope;
			foam = max(foam, event_foam * envelope * smoothstep(0.15, 1.0, abs(amplitude)));
		} else {
			float along = dot(event_to_point, travel) * dist;
			vec3 lateral_axis = normalize(cross(event_dir, travel));
			float side = dot(event_to_point, lateral_axis) * dist;
			float behind = max(-along, 0.0);
			if (behind <= 0.0) continue;
			float target_side = behind * KELVIN_TAN;
			float ridge_dist = abs(abs(side) - target_side);
			float divergent = exp(-ridge_dist / max(radius * 0.55, 0.7));
			float centre = exp(-abs(side) / max(radius * 0.7, 0.7));
			float trail_decay = exp(-behind / max(u_interaction_range_m * 0.75, 1.0));
			float age_decay = exp(-age * 0.018);
			float envelope = (divergent * 0.85 + centre * 0.35) * trail_decay * age_decay;
			float phase = (behind + abs(side) * 0.35 - speed * age * 0.12) * k;
			height += amplitude * sin(phase) * envelope;
			vec3 wake_dir = ocean_safe_tangent(d, -travel + lateral_axis * sign(side) * KELVIN_TAN);
			slope += wake_dir * amplitude * k * cos(phase) * envelope;
			foam = max(foam, event_foam * max(divergent * 0.65, centre) * trail_decay * age_decay);
		}
	}
}

float ocean_micro_phase_warp(vec2 plane_m) {
	float a = sin(dot(plane_m, vec2(0.033, 0.021)) + u_time_s * 0.21);
	float b = sin(dot(plane_m, vec2(-0.018, 0.037)) - u_time_s * 0.13 + 1.9);
	return a * 0.58 + b * 0.34;
}

// Micro waves live in stable metre-space. Only waves with a generous screen-space
// footprint are explicitly resolved; the rest become microfacet variance. A
// common low-frequency phase warp bends the resolved wavelets enough to remove
// the artificial ruler-straight corduroy pattern without inventing geometry.
void ocean_micro_wave(vec3 up, vec2 plane_m, vec3 axis, float wavelength,
		float slope_amp, float speed, float phase_offset, float phase_warp,
		inout vec3 slope, inout float unresolved_variance) {
	vec3 anchor_travel = ocean_safe_tangent(u_anchor_dir, axis);
	vec2 phase_dir = vec2(dot(anchor_travel, u_anchor_right),
		dot(anchor_travel, u_anchor_up));
	float phase_dir_len = length(phase_dir);
	if (phase_dir_len < 1e-6) return;
	phase_dir /= phase_dir_len;
	float coord_m = dot(plane_m, phase_dir);
	float footprint_m = max(fwidth(coord_m), 1e-6);
	float pixels_per_wave = wavelength / footprint_m;
	float filter_w = smoothstep(4.0, 9.0, pixels_per_wave);
	float variance = slope_amp * slope_amp * 0.5;
	unresolved_variance += (1.0 - filter_w * filter_w) * variance;
	if (filter_w <= 0.001) return;

	float k = TAU / wavelength;
	float anchor_phase = mod(ocean_geodesic_phase_coord(u_anchor_dir, axis), wavelength) * k;
	float phase = anchor_phase + coord_m * k - u_time_s * speed
		+ phase_offset + phase_warp;
	vec3 travel = ocean_safe_tangent(up, axis);
	slope += travel * (slope_amp * cos(phase) * filter_w);
}

vec3 ocean_micro_slope(vec3 up, out float unresolved_variance) {
	vec3 slope = vec3(0.0);
	unresolved_variance = 0.0;
	float warp = ocean_micro_phase_warp(v_micro_plane);
	if (u_micro_band_count >= 1) ocean_micro_wave(up, v_micro_plane, MICRO_AXIS_A, 2.8, 0.028, 3.8, 0.3, warp, slope, unresolved_variance);
	if (u_micro_band_count >= 2) ocean_micro_wave(up, v_micro_plane, MICRO_AXIS_B, 1.3, 0.022, 5.6, 1.7, warp * 1.13, slope, unresolved_variance);
	if (u_micro_band_count >= 3) ocean_micro_wave(up, v_micro_plane, MICRO_AXIS_C, 0.62, 0.016, 7.9, 3.0, warp * 0.91, slope, unresolved_variance);
	if (u_micro_band_count >= 4) ocean_micro_wave(up, v_micro_plane, MICRO_AXIS_D, 0.29, 0.011, 10.8, 4.2, warp * 1.27, slope, unresolved_variance);
	if (u_micro_band_count >= 5) ocean_micro_wave(up, v_micro_plane, MICRO_AXIS_E, 0.13, 0.0075, 15.5, 2.4, warp * 0.83, slope, unresolved_variance);
	if (u_micro_band_count >= 6) ocean_micro_wave(up, v_micro_plane, MICRO_AXIS_F, 0.055, 0.0045, 22.0, 5.1, warp * 1.41, slope, unresolved_variance);
	return slope;
}

void vertex() {
	// UV carries the original logical grid coordinate. Sector meshes are compacted
	// on the CPU, so local VERTEX_ID is deliberately not used for addressing.
	vec2 cell = UV - vec2(u_grid_cells * 0.5);
	int level = int(floor(INSTANCE_CUSTOM.x + 0.5));
	float spacing = ocean_ring_spacing(level);
	vec2 cell_offset_m = cell * spacing;
	float arc_m = length(cell_offset_m);
	float raw_theta = arc_m / max(u_planet_radius, 1.0);
	bool stable_displacement = u_stable_displacement > 0.5;
	vec2 level_center_m = round(u_lattice_center_plane / spacing) * spacing;
	vec2 anchor_offset_m = level_center_m + cell_offset_m;
	vec3 d = stable_displacement
		? ocean_direction_for_basis_offset(u_anchor_dir, u_anchor_right, u_anchor_up, anchor_offset_m)
		: ocean_direction_for_basis_offset(u_center_dir, u_center_right, u_center_up, cell_offset_m);

	float representation_morph = ocean_fine_morph(cell);
	float wave_spacing = mix(spacing, spacing * 2.0, representation_morph)
		* max(u_displacement_spacing_scale, 0.5);
	float raw_ground_h;
	float ground_h;
	vec3 landward;
	float shore_distance;
	float coast_confidence;
	if (level <= u_bathymetry_level_limit) {
		ocean_bathymetry(d, arc_m, max(wave_spacing * 2.5, 32.0), raw_ground_h,
			ground_h, landward, shore_distance, coast_confidence);
	} else {
		raw_ground_h = ocean_visible_ground_height(d, arc_m);
		ground_h = raw_ground_h;
		landward = ocean_safe_tangent(d, SWELL_AXIS_A);
		shore_distance = -max(-ground_h, 0.0) * 8.0;
		coast_confidence = 0.0;
	}
	float depth = max(-ground_h, 0.0);

	vec3 swell_a = ocean_safe_tangent(d, SWELL_AXIS_A);
	vec3 swell_b = ocean_safe_tangent(d, SWELL_AXIS_B);
	float shallow_refract = 1.0 - smoothstep(22.0, 150.0, depth);
	float refract = shallow_refract * coast_confidence;
	vec3 refracted_a = normalize(mix(swell_a, landward, refract));
	vec3 refracted_b = normalize(mix(swell_b, landward, refract * 0.88));
	float diffract_a = ocean_diffraction_weight(d, swell_a, landward, depth) * coast_confidence;
	float diffract_b = ocean_diffraction_weight(d, swell_b, landward, depth) * coast_confidence * 0.82;
	vec3 dir_a = normalize(mix(refracted_a,
		ocean_diffraction_direction(d, swell_a, landward), diffract_a * 0.52));
	vec3 dir_b = normalize(mix(refracted_b,
		ocean_diffraction_direction(d, swell_b, landward), diffract_b * 0.46));

	vec3 dir_c = normalize(mix(ocean_safe_tangent(d, SWELL_AXIS_C), landward, refract * 0.42));
	vec3 dir_d = normalize(mix(ocean_safe_tangent(d, SWELL_AXIS_D), landward, refract * 0.30));
	vec3 dir_e = normalize(mix(ocean_safe_tangent(d, SWELL_AXIS_E), landward, refract * 0.18));
	vec3 dir_f = normalize(mix(ocean_safe_tangent(d, SWELL_AXIS_F), landward, refract * 0.10));

	float phase_a = ocean_geodesic_phase_coord(d, SWELL_AXIS_A);
	float phase_b = ocean_geodesic_phase_coord(d, SWELL_AXIS_B);
	float phase_c = ocean_geodesic_phase_coord(d, SWELL_AXIS_C);
	float phase_d = ocean_geodesic_phase_coord(d, SWELL_AXIS_D);
	float phase_e = ocean_geodesic_phase_coord(d, SWELL_AXIS_E);
	float phase_f = ocean_geodesic_phase_coord(d, SWELL_AXIS_F);

	OceanWaveSample wave = ocean_empty_wave();
	ocean_add_quality_wave(0, d, dir_a, phase_a, shore_distance, depth, wave_spacing, 192.0, 1.15, 0.66, 0.0, wave);
	ocean_add_quality_wave(1, d, dir_b, phase_b, shore_distance, depth, wave_spacing, 96.0, 0.78, 0.62, 1.7, wave);
	ocean_add_quality_wave(2, d, dir_c, phase_c, shore_distance, depth, wave_spacing, 42.0, 0.32, 0.55, 3.1, wave);
	ocean_add_quality_wave(3, d, dir_d, phase_d, shore_distance, depth, wave_spacing, 18.0, 0.105, 0.48, 4.6, wave);
	ocean_add_quality_wave(4, d, dir_e, phase_e, shore_distance, depth, wave_spacing, 8.0, 0.036, 0.42, 2.2, wave);
	ocean_add_quality_wave(5, d, dir_f, phase_f, shore_distance, depth, wave_spacing, 3.4, 0.014, 0.36, 5.4, wave);

	float unresolved = 0.0;
	unresolved += ocean_band_variance(0, 192.0, 1.15 * u_wave_scale, depth, wave_spacing);
	unresolved += ocean_band_variance(1, 96.0, 0.78 * u_wave_scale, depth, wave_spacing);
	unresolved += ocean_band_variance(2, 42.0, 0.32 * u_wave_scale, depth, wave_spacing);
	unresolved += ocean_band_variance(3, 18.0, 0.105 * u_wave_scale, depth, wave_spacing);
	unresolved += ocean_band_variance(4, 8.0, 0.036 * u_wave_scale, depth, wave_spacing);
	unresolved += ocean_band_variance(5, 3.4, 0.014 * u_wave_scale, depth, wave_spacing);

	float interaction_h;
	vec3 interaction_slope;
	float interaction_foam;
	ocean_interactions(d, level, interaction_h, interaction_slope, interaction_foam);
	wave.displacement += d * interaction_h;
	wave.slope += interaction_slope;

	float radial_h = dot(wave.displacement, d);
	vec3 tangential_displacement = wave.displacement - d * radial_h;
	vec3 displaced = d * (u_planet_radius + radial_h) + tangential_displacement;

	// Only render geometry is sunk in the overlap. All lighting, atmosphere,
	// physics-facing varyings and phase coordinates retain the real water surface.
	float render_radial_h = radial_h - ocean_coarse_sink(cell, level, spacing);
	vec3 render_displaced = d * (u_planet_radius + render_radial_h) + tangential_displacement;
	v_planet = displaced;
	v_render = stable_displacement
		? u_anchor_render + ocean_stable_surface_offset(u_anchor_dir, u_anchor_right,
			u_anchor_up, anchor_offset_m, render_radial_h) + tangential_displacement
		: render_displaced - u_origin;
	v_slope = wave.slope;
	v_micro_plane = stable_displacement ? anchor_offset_m : cell_offset_m;
	v_depth = depth;
	v_breaking = wave.breaking;
	v_crest = wave.crest;
	v_compression = wave.compression;
	v_diffraction = max(diffract_a, diffract_b);
	v_unresolved_variance = unresolved;
	v_interaction_foam = interaction_foam;
	v_level = float(level);

	vec3 shadow_sun = normalize(u_cloud_shadow_sun_dir);
	float planet_vis = planet_direct_sun_visibility(displaced, shadow_sun, u_planet_radius);
	v_cloud_shadow = planet_vis > 0.001
		? cloud_shadow_transmittance(displaced, shadow_sun, u_planet_radius)
		: 0.0;

	v_valid = (u_macro_ready > 0.5 && raw_ground_h < 0.45
		&& raw_theta <= u_visible_cap_angle) ? 1.0 : 0.0;
	VERTEX = v_render;
}

void fragment() {
	if (v_valid < 0.5) discard;

	vec3 camera_planet = CAMERA_POSITION_WORLD + u_origin;
	float camera_alt = length(camera_planet) - u_planet_radius;
	if (camera_alt >= u_orbit_handoff_altitude) discard;

	vec3 up = normalize(v_planet);
	float micro_unresolved_variance;
	vec3 resolved_slope = v_slope + ocean_micro_slope(up, micro_unresolved_variance);
	vec3 n = normalize(up - resolved_slope);
	if (dot(n, up) < 0.0) n = -n;

	float depth = v_depth;
	vec3 base = water_depth_tint(depth, u_shallow, u_shelf, u_deep);
	float surf_zone = 1.0 - smoothstep(0.2, 9.0, depth);
	float shorewash = (1.0 - smoothstep(0.12, 3.8, depth)) * smoothstep(0.50, 0.93, v_crest);

	// Offshore whitecaps are driven by crest steepness, then broken into metre-scale
	// patches. Previously deep water could never foam because only the shallow
	// breaker term contributed, making the requested high sea look like polished
	// folded plastic.
	float foam_pattern = 0.5
		+ 0.25 * sin(dot(v_micro_plane, vec2(0.110, 0.073)) + u_time_s * 0.35)
		+ 0.25 * sin(dot(v_micro_plane, vec2(-0.067, 0.131)) - u_time_s * 0.23 + 2.3);
	float foam_patch = smoothstep(0.52, 0.76, foam_pattern);
	float offshore_whitecap = smoothstep(0.14, 0.30, length(v_slope))
		* smoothstep(0.60, 0.92, v_crest) * foam_patch;
	float whitecap = max(v_compression * smoothstep(0.54, 0.90, v_crest),
		offshore_whitecap * 0.82);
	float breaker_foam = v_breaking * (0.68 + 0.32 * v_compression);
	float diffracted_foam = v_diffraction * surf_zone * v_compression * 0.35;
	float foam = max(max(shorewash, whitecap * 0.70), max(breaker_foam, diffracted_foam));
	foam = max(foam, v_interaction_foam);
	foam = clamp(foam * u_foam_quality * (1.0 - smoothstep(2500.0, 14000.0, length(VERTEX))), 0.0, 1.0);
	base = mix(base, u_foam, foam * 0.90);

	vec3 sun_dir = normalize(u_sun_dir);
	vec3 skylight = planet_sky_irradiance(v_planet, sun_dir, u_planet_radius);
	float wind_rough = water_roughness_from_wind(water_wind_speed(up), 0.5);

	// Preserve both vertex-wave energy removed by clipmap LOD and fragment-wave
	// energy removed by screen-space anti-aliasing. GGX alpha is a slope scale,
	// while the public roughness parameter is sqrt(alpha); convert at that boundary
	// instead of adding tiny arbitrary perceptual roughness offsets.
	float all_unresolved_variance = max(v_unresolved_variance, 0.0)
		+ max(micro_unresolved_variance, 0.0);
	float unresolved_slope_rms = sqrt(all_unresolved_variance);
	float wind_alpha = wind_rough * wind_rough;
	float spectral_alpha = clamp(unresolved_slope_rms * 0.70, 0.0, 0.20);
	float rough = sqrt(clamp(wind_alpha + spectral_alpha, 0.003, 0.40));
	rough = clamp(rough + foam * 0.16, 0.045, 0.72);

	vec3 view_dir = normalize(camera_planet - v_planet);
	float ndotv = max(dot(n, view_dir), 0.0);
	float fres = water_fresnel(ndotv);

	// Our analytic sky is deliberately low frequency, so one roughness-aware
	// normal contraction is enough to approximate prefiltered sky reflection and
	// stop high sea normals from reflecting razor-sharp bands of haze.
	float sky_blur = smoothstep(0.22, 0.52, rough);
	vec3 reflection_n = normalize(mix(n, up, sky_blur * 0.28));
	vec3 sky_refl = water_sky_radiance(reflect(-view_dir, reflection_n), up, sun_dir);

	// Cheap water-specific subsurface/forward-scatter approximation. It is strongest
	// through thin, high crests when the sun is behind the wave relative to camera.
	float backlight = pow(max(dot(-sun_dir, view_dir), 0.0), 4.0);
	float crest_thin = smoothstep(0.55, 0.96, v_crest) * (1.0 - smoothstep(2.0, 18.0, depth));
	vec3 crest_scatter = u_scatter * backlight * crest_thin * u_crest_scatter_strength;

	vec3 atmo_t;
	vec3 atmo_l;
	aerial_perspective(camera_planet, v_planet, atmo_t, atmo_l);
	ALBEDO = base * (1.0 - fres * 0.85) * atmo_t;
	EMISSION = (base * skylight + sky_refl * fres * 0.9 + crest_scatter) * atmo_t + atmo_l;
	NORMAL = normalize((VIEW_MATRIX * vec4(n, 0.0)).xyz);
	ROUGHNESS = rough;
	SPECULAR = 0.5;
	METALLIC = 0.0;
	AO = 1.0;
	AO_LIGHT_AFFECT = 1.0;
	ALPHA = mix(0.72, 1.0, smoothstep(0.0, 6.0, depth));
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
