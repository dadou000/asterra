shader_type spatial;
render_mode cull_disabled, depth_draw_opaque, diffuse_burley, specular_schlick_ggx;

#include "res://shaders/atmosphere.gdshaderinc"
#include "res://shaders/cloud_shadow.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/gpu_terrain_height.gdshaderinc"
#include "res://shaders/water_surface.gdshaderinc"
#include "res://shaders/ocean_waves.gdshaderinc"

// Production ocean representation.
//
// Every clipmap level samples one common planet-space wave field. Fine vertices
// geomorph onto the next coarser absolute lattice before entering the geometric
// overlap. The mesh itself is a nested 2:1 square clipmap so child and parent
// boundaries are exact members of the same Cartesian lattice.
//
// Spectral LOD is representation-continuous. If a wavelength becomes too short
// for a coarse vertex grid, the missing slope continues in fragment-space with
// the same geodesic planet phase. It becomes statistical GGX variance only when
// it is genuinely too small on screen.

uniform vec3 u_origin = vec3(0.0);
uniform vec3 u_center_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_center_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_center_up = vec3(0.0, 1.0, 0.0);
uniform vec3 u_anchor_render = vec3(0.0);
uniform vec3 u_anchor_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_anchor_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_anchor_up = vec3(0.0, 1.0, 0.0);
uniform vec2 u_lattice_center_plane = vec2(0.0);
uniform float u_ocean_base_spacing = 0.75;
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

uniform vec3 u_deep : source_color = vec3(0.006, 0.028, 0.070);
uniform vec3 u_shelf : source_color = vec3(0.040, 0.215, 0.285);
uniform vec3 u_shallow : source_color = vec3(0.100, 0.405, 0.385);
uniform vec3 u_foam : source_color = vec3(0.88, 0.93, 0.96);
uniform vec3 u_scatter : source_color = vec3(0.055, 0.34, 0.30);

varying vec3 v_planet;
varying vec3 v_render;
varying vec3 v_slope;
varying vec2 v_micro_plane;
varying vec3 v_surface_dir;
varying float v_depth;
varying float v_valid;
varying float v_breaking;
varying float v_crest;
varying float v_compression;
varying float v_diffraction;
varying float v_interaction_foam;
varying float v_cloud_shadow;
varying float v_level;
varying float v_wave_spacing;
varying vec2 v_dynamic_velocity;
varying float v_dynamic_activity;
varying vec3 v_sun_atmo_t;
varying vec3 v_direct_atmo_t;

const float MAX_PROJECTED_THETA = 1.62;
const float TANGENT_APPROX_MAX_ARC_M = 90000.0;
const float TERRAIN_HALF_CELLS = 200.0;
const float TERRAIN_FINE_MORPH_START_Q = 0.875;

// Fine q=0.875 is 56 cells. The next level's inner half-width is 28 cells at
// twice the spacing, also 56 fine-cell units. Finish the morph exactly there.
const float OCEAN_FINE_MORPH_START_Q = 0.720;
const float OCEAN_FINE_MORPH_END_Q = 0.875;
const float OCEAN_RING_INNER_Q = 0.4375;
const float OCEAN_RING_HANDOFF_Q = 0.50;

const vec3 SWELL_AXIS_A = vec3(0.827, 0.201, 0.525);
const vec3 SWELL_AXIS_B = vec3(-0.436, 0.331, 0.837);
const vec3 SWELL_AXIS_C = vec3(0.153, 0.879, 0.451);
const vec3 SWELL_AXIS_D = vec3(-0.741, 0.529, -0.413);
const vec3 SWELL_AXIS_E = vec3(0.336, -0.612, 0.716);
const vec3 SWELL_AXIS_F = vec3(-0.218, -0.771, -0.598);

// Small normal-only sidebands fill the sparse directional gaps between the six
// authoritative displacement waves. They are intentionally too small to change
// buoyancy, but they stop a low sun from revealing the six-wave basis as a set of
// perfectly parallel white bars. Cox-Munk's measured slope PDF likewise requires
// a broad spectrum rather than a handful of discrete frequencies.
const vec3 CHOP_AXIS_A = vec3(0.706, 0.520, -0.481);
const vec3 CHOP_AXIS_B = vec3(-0.592, 0.694, 0.413);
const vec3 CHOP_AXIS_C = vec3(0.415, -0.323, 0.851);
const vec3 CHOP_AXIS_D = vec3(-0.864, -0.116, 0.490);
const vec3 CHOP_AXIS_E = vec3(0.104, 0.966, 0.236);
const vec3 CHOP_AXIS_F = vec3(0.731, -0.581, -0.357);

const float KELVIN_TAN = 0.35355339;
const float WATER_SURFACE_TENSION_OVER_DENSITY = 7.28e-5;

float ocean_ring_spacing(int level) {
	return u_ocean_base_spacing * exp2(float(level));
}

float ocean_smoother01(float x) {
	x = clamp(x, 0.0, 1.0);
	return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

float ocean_screen_band_weight(float pixels_per_wave) {
	return ocean_smoother01((pixels_per_wave - 2.0) / 12.0);
}

float ocean_ring_q(vec2 cell) {
	float half_cells = max(u_grid_cells * 0.5, 1.0);
	return max(abs(cell.x), abs(cell.y)) / half_cells;
}

float ocean_fine_morph(vec2 cell) {
	float q = ocean_ring_q(cell);
	return ocean_smoother01((q - OCEAN_FINE_MORPH_START_Q)
		/ (OCEAN_FINE_MORPH_END_Q - OCEAN_FINE_MORPH_START_Q));
}

vec2 ocean_parent_lattice(vec2 anchor_offset_m, float spacing) {
	float parent_spacing = spacing * 2.0;
	vec2 parent_cell = floor(anchor_offset_m / parent_spacing + vec2(0.5));
	return parent_cell * parent_spacing;
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

float ocean_hash21(vec2 p) {
	vec3 p3 = fract(vec3(p.x, p.y, p.x) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

vec3 ocean_value_noise_grad(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	float a = ocean_hash21(i);
	float b = ocean_hash21(i + vec2(1.0, 0.0));
	float c = ocean_hash21(i + vec2(0.0, 1.0));
	float d = ocean_hash21(i + vec2(1.0, 1.0));
	vec2 u = f * f * (3.0 - 2.0 * f);
	vec2 du = 6.0 * f * (1.0 - f);
	float nx0 = mix(a, b, u.x);
	float nx1 = mix(c, d, u.x);
	float value = mix(nx0, nx1, u.y);
	float dx = mix(b - a, d - c, u.y) * du.x;
	float dy = (nx1 - nx0) * du.y;
	return vec3(value * 2.0 - 1.0, dx * 2.0, dy * 2.0);
}

float ocean_noise01(vec2 p) {
	return ocean_value_noise_grad(p).x * 0.5 + 0.5;
}

vec2 ocean_wind_plane_dir(vec3 up) {
	vec3 wind_world = water_wind_direction(up);
	vec2 wind_plane = vec2(dot(wind_world, u_anchor_right), dot(wind_world, u_anchor_up));
	float l2 = dot(wind_plane, wind_plane);
	if (l2 < 1e-8) return vec2(1.0, 0.0);
	return wind_plane * inversesqrt(l2);
}

float ocean_short_wave_phase_speed(float wavelength, float depth) {
	float k = TAU / max(wavelength, 0.02);
	float omega2 = (OCEAN_G * k
		+ WATER_SURFACE_TENSION_OVER_DENSITY * k * k * k)
		* tanh(k * max(depth, 0.05));
	return sqrt(max(omega2, 1e-6)) / k;
}

void ocean_micro_noise_band(vec3 up, vec2 plane_m, float wavelength,
		float slope_amp, float spread_angle, float phase_offset,
		inout vec3 slope, inout float unresolved_variance) {
	vec2 wind_dir = ocean_wind_plane_dir(up);
	float ca = cos(spread_angle);
	float sa = sin(spread_angle);
	vec2 dir = normalize(vec2(
		ca * wind_dir.x - sa * wind_dir.y,
		sa * wind_dir.x + ca * wind_dir.y));
	vec2 across_dir = vec2(-dir.y, dir.x);

	float phase_speed = ocean_short_wave_phase_speed(wavelength, v_depth);
	float along_m = dot(plane_m, dir) - phase_speed * u_time_s;
	float across_m = dot(plane_m, across_dir);
	float cycles = along_m / max(wavelength, 0.02);
	float footprint = max(fwidth(cycles), 1e-6);
	float pixels_per_wave = 1.0 / footprint;
	float filter_w = ocean_screen_band_weight(pixels_per_wave);

	float scaled_slope = slope_amp * u_wave_scale;
	float variance = scaled_slope * scaled_slope * 0.5;
	unresolved_variance += (1.0 - filter_w * filter_w) * variance;
	if (filter_w <= 0.001 || scaled_slope <= 1e-6) return;

	vec2 warp_q = vec2(
		along_m / max(wavelength * 3.6, 0.05),
		across_m / max(wavelength * 2.2, 0.05))
		+ vec2(phase_offset * 0.73, phase_offset * -0.41);
	vec3 ng = ocean_value_noise_grad(warp_q);
	float phase = TAU * cycles + ng.x * 0.95 + phase_offset;
	float along_slope = scaled_slope * (0.82 + 0.18 * ng.x) * cos(phase);
	float cross_slope = scaled_slope * 0.16 * ng.z * sin(phase);
	vec2 local_slope = dir * along_slope + across_dir * cross_slope;

	vec3 tx = ocean_safe_tangent(up, u_anchor_right);
	vec3 ty = normalize(cross(up, tx));
	if (dot(ty, u_anchor_up) < 0.0) ty = -ty;
	slope += (tx * local_slope.x + ty * local_slope.y) * filter_w;
}

vec3 ocean_micro_slope(vec3 up, out float unresolved_variance) {
	vec3 slope = vec3(0.0);
	unresolved_variance = 0.0;
	if (u_micro_band_count >= 1) ocean_micro_noise_band(up, v_micro_plane, 4.8, 0.0300, 0.18, 0.7, slope, unresolved_variance);
	if (u_micro_band_count >= 2) ocean_micro_noise_band(up, v_micro_plane, 2.25, 0.0240, -0.28, 2.1, slope, unresolved_variance);
	if (u_micro_band_count >= 3) ocean_micro_noise_band(up, v_micro_plane, 1.05, 0.0180, 0.42, 4.4, slope, unresolved_variance);
	if (u_micro_band_count >= 4) ocean_micro_noise_band(up, v_micro_plane, 0.49, 0.0130, -0.55, 1.2, slope, unresolved_variance);
	if (u_micro_band_count >= 5) ocean_micro_noise_band(up, v_micro_plane, 0.23, 0.0090, 0.68, 3.5, slope, unresolved_variance);
	if (u_micro_band_count >= 6) ocean_micro_noise_band(up, v_micro_plane, 0.105, 0.0065, -0.82, 5.7, slope, unresolved_variance);
	return slope;
}

void ocean_visual_chop_band(vec3 up, vec3 axis, float wavelength,
		float slope_amp, float phase_offset,
		inout vec3 slope, inout float unresolved_variance) {
	vec3 surface_dir = normalize(v_surface_dir);
	float phase_coord = ocean_geodesic_phase_coord(surface_dir, axis);
	float footprint_m = max(fwidth(phase_coord), 1e-6);
	float pixels_per_wave = wavelength / footprint_m;
	float screen_w = ocean_screen_band_weight(pixels_per_wave);

	float shallow_w = smoothstep(0.35, max(1.2, wavelength * 0.18), v_depth);
	float s = slope_amp * u_wave_scale * shallow_w;
	unresolved_variance += (1.0 - screen_w * screen_w) * s * s * 0.5;
	if (screen_w <= 0.001 || s <= 1e-6) return;

	float k = TAU / wavelength;
	float omega = ocean_dispersion(k, max(v_depth, 0.05));
	float phase = phase_coord * k - omega * u_time_s + phase_offset;
	vec3 travel = ocean_safe_tangent(up, axis);
	slope += travel * (s * cos(phase) * screen_w);
}

vec3 ocean_directional_spread_slope(vec3 up, out float unresolved_variance) {
	vec3 slope = vec3(0.0);
	unresolved_variance = 0.0;
	if (u_micro_band_count >= 1) ocean_visual_chop_band(up, CHOP_AXIS_A, 24.0, 0.0130, 0.4, slope, unresolved_variance);
	if (u_micro_band_count >= 2) ocean_visual_chop_band(up, CHOP_AXIS_B, 15.0, 0.0120, 2.6, slope, unresolved_variance);
	if (u_micro_band_count >= 3) ocean_visual_chop_band(up, CHOP_AXIS_C, 9.5, 0.0110, 4.1, slope, unresolved_variance);
	if (u_micro_band_count >= 4) ocean_visual_chop_band(up, CHOP_AXIS_D, 6.0, 0.0095, 1.4, slope, unresolved_variance);
	if (u_micro_band_count >= 5) ocean_visual_chop_band(up, CHOP_AXIS_E, 3.8, 0.0075, 5.1, slope, unresolved_variance);
	if (u_micro_band_count >= 6) ocean_visual_chop_band(up, CHOP_AXIS_F, 2.4, 0.0060, 3.2, slope, unresolved_variance);
	return slope;
}

void ocean_residual_wave_band(vec3 up, vec3 axis,
		float wavelength, float base_amp, float phase_offset, float depth,
		float geometry_spacing, inout vec3 slope, inout float unresolved_variance) {
	float geom_w = ocean_wave_band_weight(wavelength, geometry_spacing);
	float residual = 1.0 - geom_w;
	if (residual <= 0.001) return;

	vec3 surface_dir = normalize(v_surface_dir);
	float phase_coord = ocean_geodesic_phase_coord(surface_dir, axis);
	float footprint_m = max(fwidth(phase_coord), 1e-6);
	float pixels_per_wave = wavelength / footprint_m;
	float screen_w = ocean_screen_band_weight(pixels_per_wave);

	float amp = min(base_amp * u_wave_scale * ocean_shoal_gain(depth, wavelength),
		max(0.015, BREAKER_GAMMA * depth * 0.5));
	float k = TAU / wavelength;
	float residual_slope = amp * k * residual;

	float deep_w = smoothstep(18.0, 95.0, depth);
	float resolved_w = screen_w * deep_w;
	unresolved_variance += (1.0 - resolved_w * resolved_w)
		* residual_slope * residual_slope * 0.5;
	if (resolved_w <= 0.001) return;

	float omega = ocean_dispersion(k, depth)
		* ocean_coast_frequency_scale(depth, wavelength);
	float phase = phase_coord * k - omega * u_time_s + phase_offset;
	vec3 travel = ocean_safe_tangent(up, axis);
	slope += travel * (residual_slope * cos(phase) * resolved_w);
}

vec3 ocean_residual_spectrum(vec3 up, out float unresolved_variance) {
	vec3 slope = vec3(0.0);
	unresolved_variance = 0.0;
	float spacing = max(v_wave_spacing, 0.01);
	if (u_quality_band_count >= 1) ocean_residual_wave_band(up, SWELL_AXIS_A, 192.0, 1.15, 0.0, v_depth, spacing, slope, unresolved_variance);
	if (u_quality_band_count >= 2) ocean_residual_wave_band(up, SWELL_AXIS_B, 96.0, 0.78, 1.7, v_depth, spacing, slope, unresolved_variance);
	if (u_quality_band_count >= 3) ocean_residual_wave_band(up, SWELL_AXIS_C, 42.0, 0.32, 3.1, v_depth, spacing, slope, unresolved_variance);
	if (u_quality_band_count >= 4) ocean_residual_wave_band(up, SWELL_AXIS_D, 18.0, 0.105, 4.6, v_depth, spacing, slope, unresolved_variance);
	if (u_quality_band_count >= 5) ocean_residual_wave_band(up, SWELL_AXIS_E, 8.0, 0.036, 2.2, v_depth, spacing, slope, unresolved_variance);
	if (u_quality_band_count >= 6) ocean_residual_wave_band(up, SWELL_AXIS_F, 3.4, 0.014, 5.4, v_depth, spacing, slope, unresolved_variance);
	return slope;
}

vec3 ocean_vertical_optical_depth() {
	return u_rayleigh_coeff * u_rayleigh_scale_height
		+ vec3(u_mie_coeff * u_mie_scale_height)
		+ u_ozone_coeff * u_ozone_width;
}

vec3 ocean_surface_sun_transmittance(vec3 p, vec3 sun_dir) {
	float mu = dot(normalize(p), normalize(sun_dir));
	if (mu <= -0.015) return vec3(0.0);
	float m = max(mu, 0.001);
	float zenith_deg = degrees(acos(clamp(m, 0.0, 1.0)));
	float denom = m + 0.50572 * pow(max(96.07995 - zenith_deg, 0.1), -1.6364);
	float air_mass = min(1.0 / max(denom, 0.025), 38.0);
	return exp(-ocean_vertical_optical_depth() * air_mass);
}

vec3 ocean_view_transmittance(vec3 surface, vec3 camera) {
	float distance_m = length(camera - surface);
	float h = max((length(surface) + length(camera)) * 0.5 - u_planet_radius, 0.0);
	float rayleigh_density = exp(-h / max(u_rayleigh_scale_height, 1.0));
	float mie_density = exp(-h / max(u_mie_scale_height, 1.0));
	vec3 extinction = u_rayleigh_coeff * rayleigh_density
		+ vec3(u_mie_coeff * mie_density);
	return exp(-extinction * min(distance_m, 80000.0));
}

void vertex() {
	vec2 cell = UV - vec2(u_grid_cells * 0.5);
	int level = int(floor(INSTANCE_CUSTOM.x + 0.5));
	float spacing = ocean_ring_spacing(level);
	float ring_q = ocean_ring_q(cell);
	float representation_morph = ocean_fine_morph(cell);

	vec2 level_center_m = floor(u_lattice_center_plane / spacing + vec2(0.5)) * spacing;
	vec2 anchor_offset_m = level_center_m + cell * spacing;
	vec2 parent_offset_m = ocean_parent_lattice(anchor_offset_m, spacing);
	vec2 sample_anchor_m = mix(anchor_offset_m, parent_offset_m, representation_morph);

	vec2 observer_offset_m = sample_anchor_m - u_lattice_center_plane;
	float arc_m = length(observer_offset_m);
	float raw_theta = arc_m / max(u_planet_radius, 1.0);
	bool stable_displacement = u_stable_displacement > 0.5;
	vec3 d = stable_displacement
		? ocean_direction_for_basis_offset(u_anchor_dir, u_anchor_right, u_anchor_up, sample_anchor_m)
		: ocean_direction_for_basis_offset(u_center_dir, u_center_right, u_center_up, observer_offset_m);

	float wave_spacing = mix(spacing, spacing * 2.0, representation_morph)
		* max(u_displacement_spacing_scale, 0.5);
	float raw_ground_h;
	float ground_h;
	vec3 landward;
	float shore_distance;
	float coast_confidence;
	vec3 simple_landward = ocean_safe_tangent(d, SWELL_AXIS_A);
	if (level <= u_bathymetry_level_limit) {
		ocean_bathymetry(d, arc_m, max(wave_spacing * 2.5, 32.0), raw_ground_h,
			ground_h, landward, shore_distance, coast_confidence);
		if (level == u_bathymetry_level_limit) {
			ground_h = mix(ground_h, raw_ground_h, representation_morph);
			landward = normalize(mix(landward, simple_landward, representation_morph));
			shore_distance = mix(shore_distance,
				-max(-raw_ground_h, 0.0) * 8.0, representation_morph);
			coast_confidence *= 1.0 - representation_morph;
		}
	} else {
		raw_ground_h = ocean_visible_ground_height(d, arc_m);
		ground_h = raw_ground_h;
		landward = simple_landward;
		shore_distance = -max(-ground_h, 0.0) * 8.0;
		coast_confidence = 0.0;
	}

	WaterDynamicSurfaceSample dynamic_surface = water_dynamic_surface_sample(sample_anchor_m);
	vec2 dynamic_gradient = water_dynamic_surface_gradient(sample_anchor_m);
	float dynamic_height = dynamic_surface.height * dynamic_surface.valid;
	float depth = max(dynamic_height - ground_h, 0.0);

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
	vec3 dynamic_tx = ocean_safe_tangent(d, u_anchor_right);
	vec3 dynamic_ty = normalize(cross(d, dynamic_tx));
	if (dot(dynamic_ty, u_anchor_up) < 0.0) dynamic_ty = -dynamic_ty;
	wave.displacement += d * dynamic_height;
	wave.slope += dynamic_tx * dynamic_gradient.x + dynamic_ty * dynamic_gradient.y;

	ocean_add_quality_wave(0, d, dir_a, phase_a, shore_distance, depth, wave_spacing, 192.0, 1.15, 0.66, 0.0, wave);
	ocean_add_quality_wave(1, d, dir_b, phase_b, shore_distance, depth, wave_spacing, 96.0, 0.78, 0.62, 1.7, wave);
	ocean_add_quality_wave(2, d, dir_c, phase_c, shore_distance, depth, wave_spacing, 42.0, 0.32, 0.55, 3.1, wave);
	ocean_add_quality_wave(3, d, dir_d, phase_d, shore_distance, depth, wave_spacing, 18.0, 0.105, 0.48, 4.6, wave);
	ocean_add_quality_wave(4, d, dir_e, phase_e, shore_distance, depth, wave_spacing, 8.0, 0.036, 0.42, 2.2, wave);
	ocean_add_quality_wave(5, d, dir_f, phase_f, shore_distance, depth, wave_spacing, 3.4, 0.014, 0.36, 5.4, wave);

	float interaction_h;
	vec3 interaction_slope;
	float interaction_foam;
	ocean_interactions(d, level, interaction_h, interaction_slope, interaction_foam);
	if (level == u_interaction_vertex_level) {
		float interaction_lod = 1.0 - representation_morph;
		interaction_h *= interaction_lod;
		interaction_slope *= interaction_lod;
		interaction_foam *= interaction_lod;
	}
	wave.displacement += d * interaction_h;
	wave.slope += interaction_slope;

	float radial_h = dot(wave.displacement, d);
	vec3 tangential_displacement = wave.displacement - d * radial_h;
	vec3 displaced = d * (u_planet_radius + radial_h) + tangential_displacement;

	float seam_underlay = 0.0;
	if (level > 0 && ring_q < OCEAN_RING_HANDOFF_Q) {
		float seam_t = 1.0 - ocean_smoother01((ring_q - OCEAN_RING_INNER_Q)
			/ (OCEAN_RING_HANDOFF_Q - OCEAN_RING_INNER_Q));
		seam_underlay = seam_t * clamp(spacing * 0.000125, 0.001, 0.008);
	}
	float render_radial_h = radial_h - seam_underlay;

	v_planet = displaced;
	v_render = stable_displacement
		? u_anchor_render + ocean_stable_surface_offset(u_anchor_dir, u_anchor_right,
			u_anchor_up, sample_anchor_m, render_radial_h) + tangential_displacement
		: d * (u_planet_radius + render_radial_h) + tangential_displacement - u_origin;
	v_slope = wave.slope;
	v_micro_plane = sample_anchor_m;
	v_surface_dir = d;
	v_depth = depth;
	v_breaking = wave.breaking;
	v_crest = wave.crest;
	v_compression = wave.compression;
	v_diffraction = max(diffract_a, diffract_b);
	v_interaction_foam = max(interaction_foam, dynamic_surface.activity);
	v_level = float(level);
	v_wave_spacing = wave_spacing;
	v_dynamic_velocity = dynamic_surface.velocity;
	v_dynamic_activity = dynamic_surface.activity;

	vec3 shadow_sun = normalize(u_cloud_shadow_sun_dir);
	float planet_vis = planet_direct_sun_visibility(displaced, shadow_sun, u_planet_radius);
	v_cloud_shadow = planet_vis > 0.001
		? cloud_shadow_transmittance(displaced, shadow_sun, u_planet_radius)
		: 0.0;

	vec3 camera_planet_vertex = CAMERA_POSITION_WORLD + u_origin;
	v_sun_atmo_t = ocean_surface_sun_transmittance(displaced, shadow_sun);
	v_direct_atmo_t = v_sun_atmo_t
		* ocean_view_transmittance(displaced, camera_planet_vertex);

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
	float residual_unresolved_variance;
	vec3 residual_slope = ocean_residual_spectrum(up, residual_unresolved_variance);
	float spread_unresolved_variance;
	vec3 spread_slope = ocean_directional_spread_slope(up, spread_unresolved_variance);
	float micro_unresolved_variance;
	vec3 micro_slope = ocean_micro_slope(up, micro_unresolved_variance);
	vec3 resolved_slope = v_slope + residual_slope + spread_slope + micro_slope;
	vec3 n = normalize(up - resolved_slope);
	if (dot(n, up) < 0.0) n = -n;

	float depth = v_depth;
	vec3 base = water_depth_tint(depth, u_shallow, u_shelf, u_deep);
	float surf_zone = 1.0 - smoothstep(0.2, 9.0, depth);
	float shorewash = (1.0 - smoothstep(0.12, 3.8, depth)) * smoothstep(0.50, 0.93, v_crest);

	vec2 foam_wind = ocean_wind_plane_dir(up);
	vec2 foam_cross = vec2(-foam_wind.y, foam_wind.x);
	float foam_a = ocean_noise01((v_micro_plane - foam_wind * (0.22 * u_time_s)) / 18.0);
	float foam_b = ocean_noise01((v_micro_plane
		- foam_wind * (0.11 * u_time_s) + foam_cross * (0.035 * u_time_s)) / 6.5
		+ vec2(17.3, -9.1));
	float foam_pattern = foam_a * 0.64 + foam_b * 0.36;
	float foam_patch = smoothstep(0.47, 0.70, foam_pattern);

	float wind_speed = water_wind_speed(up);
	float wind_state = smoothstep(5.0, 17.0, wind_speed);
	float cap_low = mix(0.105, 0.040, wind_state);
	float cap_high = mix(0.210, 0.095, wind_state);
	float offshore_whitecap = smoothstep(cap_low, cap_high, length(v_slope + residual_slope))
		* smoothstep(0.53, 0.88, v_crest) * foam_patch;
	float compression_cap = smoothstep(0.22, 0.72, v_compression)
		* smoothstep(0.50, 0.86, v_crest) * foam_patch;
	float whitecap = max(offshore_whitecap, compression_cap);
	float breaker_foam = v_breaking * (0.68 + 0.32 * v_compression);
	float diffracted_foam = v_diffraction * surf_zone * v_compression * 0.35;
	float dynamic_flow_foam = v_dynamic_activity
		* smoothstep(0.35, 3.0, length(v_dynamic_velocity));
	float foam = max(max(shorewash, whitecap * 0.78), max(breaker_foam, diffracted_foam));
	foam = max(foam, max(v_interaction_foam, dynamic_flow_foam));
	foam = clamp(foam * u_foam_quality * (1.0 - smoothstep(3500.0, 18000.0, length(VERTEX))), 0.0, 1.0);
	base = mix(base, u_foam, foam * 0.88);

	float wind_rough = water_roughness_from_wind(wind_speed, 0.5);
	float all_unresolved_variance = max(residual_unresolved_variance, 0.0)
		+ max(spread_unresolved_variance, 0.0)
		+ max(micro_unresolved_variance, 0.0);
	float unresolved_slope_rms = sqrt(all_unresolved_variance);
	float wind_alpha = wind_rough * wind_rough;
	float spectral_alpha = clamp(unresolved_slope_rms * 0.55, 0.0, 0.22);
	float combined_alpha = sqrt(wind_alpha * wind_alpha
		+ spectral_alpha * spectral_alpha);
	float rough = sqrt(clamp(combined_alpha, 0.004, 0.36));
	rough = clamp(rough + foam * 0.20, 0.068, 0.82);

	vec3 sun_dir = normalize(u_sun_dir);
	vec3 view_dir = normalize(camera_planet - v_planet);
	float ndotv = max(dot(n, view_dir), 0.0);
	float fres = water_fresnel(ndotv);

	float backlight = pow(max(dot(-sun_dir, view_dir), 0.0), 4.0);
	float crest_thin = smoothstep(0.64, 0.97, v_crest)
		* (0.35 + 0.65 * smoothstep(0.08, 0.65, v_compression))
		* (1.0 - foam * 0.55);
	vec3 crest_scatter = u_scatter * backlight * crest_thin * u_crest_scatter_strength;

	vec3 atmo_t;
	vec3 atmo_l;
	aerial_perspective(camera_planet, v_planet, atmo_t, atmo_l);

	ALBEDO = base * (1.0 - fres) * atmo_t;
	EMISSION = crest_scatter * atmo_t + atmo_l;
	NORMAL = normalize((VIEW_MATRIX * vec4(n, 0.0)).xyz);
	ROUGHNESS = rough;
	SPECULAR = WATER_GODOT_SPECULAR;
	METALLIC = 0.0;
	AO = 1.0;
	AO_LIGHT_AFFECT = 1.0;

	float view_t = clamp(dot(atmo_t, vec3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
	RADIANCE = vec4(vec3(0.0), 1.0 - view_t);
}

void light() {
	float visibility = 1.0;
	vec3 diffuse_t = vec3(1.0);
	vec3 specular_t = vec3(1.0);
	if (LIGHT_IS_DIRECTIONAL) {
		visibility = planet_direct_sun_visibility(v_planet, normalize(u_sun_dir), u_planet_radius);
		visibility *= clamp(v_cloud_shadow, 0.0, 1.0);
		diffuse_t = clamp(v_sun_atmo_t, vec3(0.0), vec3(1.0));
		specular_t = clamp(v_direct_atmo_t, vec3(0.0), vec3(1.0));
	}

	float volume_diffuse = mix(0.18, 0.035, smoothstep(0.5, 30.0, v_depth));
	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	DIFFUSE_LIGHT += ndotl * ATTENUATION * LIGHT_COLOR * diffuse_t
		* visibility * volume_diffuse / PI;

	vec3 wind_world = water_wind_direction(normalize(v_planet));
	vec3 wind_view = normalize((VIEW_MATRIX * vec4(wind_world, 0.0)).xyz);
	float stretch = water_cx_munk_stretch(water_wind_speed(normalize(v_planet)));
	float spec = water_specular_direct_anisotropic(
		NORMAL, wind_view, VIEW, LIGHT, ROUGHNESS, stretch);
	SPECULAR_LIGHT += LIGHT_COLOR * specular_t * ATTENUATION * visibility
		* spec * SPECULAR_AMOUNT;
}