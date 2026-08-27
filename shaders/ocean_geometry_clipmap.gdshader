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
// geomorph onto the next coarser absolute lattice before entering the actual
// geometric overlap. The overlap is kept as a watertight underlay instead of
// being cut with fragment discard, because independently rasterized circular
// grids do not have identical edge topology.
//
// Spectral LOD is also representation-continuous. If a wavelength becomes too
// short for a coarse vertex grid, the missing slope continues in fragment-space
// with the same world phase. It becomes statistical GGX variance only when it is
// genuinely too small on screen. A ring therefore changes tessellation, not the
// apparent phase/orientation or the existence of surface detail.

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
varying vec3 v_direct_atmo_t;

const float MAX_PROJECTED_THETA = 1.62;
const float TANGENT_APPROX_MAX_ARC_M = 90000.0;
const float TERRAIN_HALF_CELLS = 200.0;
const float TERRAIN_FINE_MORPH_START_Q = 0.875;

// Finish geomorphing before the geometric overlap begins. The old 0.875..1.0
// morph meant the fine and coarse surfaces represented different spectra through
// most of their overlap. By q=0.875 the child is now exactly the parent surface.
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
const float KELVIN_TAN = 0.35355339;

float ocean_ring_spacing(int level) {
	return u_ocean_base_spacing * exp2(float(level));
}

float ocean_smoother01(float x) {
	x = clamp(x, 0.0, 1.0);
	return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

float ocean_ring_q(vec2 cell) {
	return length(cell) / max(u_grid_cells * 0.5, 1.0);
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

// Stable stochastic micro-surface ------------------------------------------------

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

void ocean_micro_noise_band(vec3 up, vec2 plane_m, float wavelength,
		float slope_amp, vec2 flow_mps, float angle,
		inout vec3 slope, inout float unresolved_variance) {
	float ca = cos(angle);
	float sa = sin(angle);
	vec2 rotated = vec2(
		ca * plane_m.x - sa * plane_m.y,
		sa * plane_m.x + ca * plane_m.y);
	vec2 q = (rotated + flow_mps * u_time_s) / max(wavelength, 0.02);

	vec2 fw = fwidth(q);
	float footprint = max(max(fw.x, fw.y), 1e-6);
	float filter_w = 1.0 - smoothstep(0.14, 0.34, footprint);
	float variance = slope_amp * slope_amp * 0.62;
	unresolved_variance += (1.0 - filter_w * filter_w) * variance;
	if (filter_w <= 0.001) return;

	vec3 ng = ocean_value_noise_grad(q);
	vec2 gp = ng.yz;
	vec2 g = vec2(
		ca * gp.x + sa * gp.y,
		-sa * gp.x + ca * gp.y);
	vec3 tx = ocean_safe_tangent(up, u_anchor_right);
	vec3 ty = normalize(cross(up, tx));
	if (dot(ty, u_anchor_up) < 0.0) ty = -ty;
	slope += (tx * g.x + ty * g.y) * (slope_amp * filter_w);
}

vec3 ocean_micro_slope(vec3 up, out float unresolved_variance) {
	vec3 slope = vec3(0.0);
	unresolved_variance = 0.0;
	if (u_micro_band_count >= 1) ocean_micro_noise_band(up, v_micro_plane, 4.8, 0.026, vec2(0.44, 0.18), 0.24, slope, unresolved_variance);
	if (u_micro_band_count >= 2) ocean_micro_noise_band(up, v_micro_plane, 2.25, 0.021, vec2(-0.16, 0.62), 1.07, slope, unresolved_variance);
	if (u_micro_band_count >= 3) ocean_micro_noise_band(up, v_micro_plane, 1.05, 0.016, vec2(0.73, -0.21), 2.18, slope, unresolved_variance);
	if (u_micro_band_count >= 4) ocean_micro_noise_band(up, v_micro_plane, 0.49, 0.012, vec2(-0.48, -0.67), 0.71, slope, unresolved_variance);
	if (u_micro_band_count >= 5) ocean_micro_noise_band(up, v_micro_plane, 0.23, 0.0085, vec2(0.91, 0.34), 1.63, slope, unresolved_variance);
	if (u_micro_band_count >= 6) ocean_micro_noise_band(up, v_micro_plane, 0.105, 0.0060, vec2(-0.83, 0.78), 2.74, slope, unresolved_variance);
	return slope;
}

// Residual geometric spectrum ----------------------------------------------------
//
// Vertex LOD is allowed to remove displacement, but removing its slope too makes
// a coarse ring look like a different material. Reconstruct that missing slope
// in fragment-space from the same anchored phase. Only screen-unresolvable energy
// is converted into roughness.

void ocean_residual_wave_band(vec3 up, vec2 plane_m, vec3 axis,
		float wavelength, float base_amp, float phase_offset, float depth,
		float geometry_spacing, inout vec3 slope, inout float unresolved_variance) {
	float geom_w = ocean_wave_band_weight(wavelength, geometry_spacing);
	float residual = 1.0 - geom_w;
	if (residual <= 0.001) return;

	vec3 anchor_travel = ocean_safe_tangent(u_anchor_dir, axis);
	vec2 phase_dir = vec2(dot(anchor_travel, u_anchor_right),
		dot(anchor_travel, u_anchor_up));
	float dir_len = length(phase_dir);
	if (dir_len <= 1e-6) return;
	phase_dir /= dir_len;

	float coord_m = dot(plane_m, phase_dir);
	float footprint_m = max(fwidth(coord_m), 1e-6);
	float pixels_per_wave = wavelength / footprint_m;
	float screen_w = smoothstep(3.5, 8.0, pixels_per_wave);

	float amp = min(base_amp * u_wave_scale * ocean_shoal_gain(depth, wavelength),
		max(0.015, BREAKER_GAMMA * depth * 0.5));
	float k = TAU / wavelength;
	float residual_slope = amp * k * residual;

	// The coast-steered wave directions live in vertex-space. Keep this normal
	// continuation for open/deep water, where the planet-space phase axis is the
	// physical direction. Near shore, unresolved energy becomes roughness instead
	// of fighting the refraction field.
	float deep_w = smoothstep(18.0, 95.0, depth);
	float resolved_w = screen_w * deep_w;
	unresolved_variance += (1.0 - resolved_w * resolved_w)
		* residual_slope * residual_slope * 0.5;
	if (resolved_w <= 0.001) return;

	float anchor_coord = ocean_geodesic_phase_coord(u_anchor_dir, axis);
	float omega = ocean_dispersion(k, depth)
		* ocean_coast_frequency_scale(depth, wavelength);
	float phase = (anchor_coord + coord_m) * k - omega * u_time_s + phase_offset;
	vec3 travel = ocean_safe_tangent(up, axis);
	slope += travel * (residual_slope * cos(phase) * resolved_w);
}

vec3 ocean_residual_spectrum(vec3 up, out float unresolved_variance) {
	vec3 slope = vec3(0.0);
	unresolved_variance = 0.0;
	float spacing = max(v_wave_spacing, 0.01);
	if (u_quality_band_count >= 1) ocean_residual_wave_band(up, v_micro_plane, SWELL_AXIS_A, 192.0, 1.15, 0.0, v_depth, spacing, slope, unresolved_variance);
	if (u_quality_band_count >= 2) ocean_residual_wave_band(up, v_micro_plane, SWELL_AXIS_B, 96.0, 0.78, 1.7, v_depth, spacing, slope, unresolved_variance);
	if (u_quality_band_count >= 3) ocean_residual_wave_band(up, v_micro_plane, SWELL_AXIS_C, 42.0, 0.32, 3.1, v_depth, spacing, slope, unresolved_variance);
	if (u_quality_band_count >= 4) ocean_residual_wave_band(up, v_micro_plane, SWELL_AXIS_D, 18.0, 0.105, 4.6, v_depth, spacing, slope, unresolved_variance);
	if (u_quality_band_count >= 5) ocean_residual_wave_band(up, v_micro_plane, SWELL_AXIS_E, 8.0, 0.036, 2.2, v_depth, spacing, slope, unresolved_variance);
	if (u_quality_band_count >= 6) ocean_residual_wave_band(up, v_micro_plane, SWELL_AXIS_F, 3.4, 0.014, 5.4, v_depth, spacing, slope, unresolved_variance);
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

	// Keep the coarse overlap instead of discarding it. The child has already
	// converged to the parent lattice/spectrum before q=0.875, so the overlap is
	// effectively the same surface. A millimetre/centimetre underlay bias prevents
	// coincident-depth flicker and fills the inevitable raster gaps between two
	// independently triangulated circular boundaries without producing a visible
	// trench like the former metre-scale sink did.
	float seam_underlay = 0.0;
	if (level > 0 && ring_q < OCEAN_RING_HANDOFF_Q) {
		float seam_t = 1.0 - ocean_smoother01((ring_q - OCEAN_RING_INNER_Q)
			/ (OCEAN_RING_HANDOFF_Q - OCEAN_RING_INNER_Q));
		seam_underlay = seam_t * clamp(spacing * 0.00025, 0.002, 0.020);
	}
	float render_radial_h = radial_h - seam_underlay;

	v_planet = displaced;
	v_render = stable_displacement
		? u_anchor_render + ocean_stable_surface_offset(u_anchor_dir, u_anchor_right,
			u_anchor_up, sample_anchor_m, render_radial_h) + tangential_displacement
		: d * (u_planet_radius + render_radial_h) + tangential_displacement - u_origin;
	v_slope = wave.slope;
	// Always use the absolute anchor plane for optical detail. This must not switch
	// coordinate systems with the debug displacement mode or the ring number.
	v_micro_plane = sample_anchor_m;
	v_depth = depth;
	v_breaking = wave.breaking;
	v_crest = wave.crest;
	v_compression = wave.compression;
	v_diffraction = max(diffract_a, diffract_b);
	v_interaction_foam = interaction_foam;
	v_level = float(level);
	v_wave_spacing = wave_spacing;

	vec3 shadow_sun = normalize(u_cloud_shadow_sun_dir);
	float planet_vis = planet_direct_sun_visibility(displaced, shadow_sun, u_planet_radius);
	v_cloud_shadow = planet_vis > 0.001
		? cloud_shadow_transmittance(displaced, shadow_sun, u_planet_radius)
		: 0.0;

	vec3 camera_planet_vertex = CAMERA_POSITION_WORLD + u_origin;
	v_direct_atmo_t = ocean_surface_sun_transmittance(displaced, shadow_sun)
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
	float micro_unresolved_variance;
	vec3 micro_slope = ocean_micro_slope(up, micro_unresolved_variance);
	vec3 resolved_slope = v_slope + residual_slope + micro_slope;
	vec3 n = normalize(up - resolved_slope);
	if (dot(n, up) < 0.0) n = -n;

	float depth = v_depth;
	vec3 base = water_depth_tint(depth, u_shallow, u_shelf, u_deep);
	float surf_zone = 1.0 - smoothstep(0.2, 9.0, depth);
	float shorewash = (1.0 - smoothstep(0.12, 3.8, depth)) * smoothstep(0.50, 0.93, v_crest);

	float foam_a = ocean_noise01(v_micro_plane / 18.0 + vec2(0.013, 0.007) * u_time_s);
	float foam_b = ocean_noise01(v_micro_plane / 6.5 + vec2(-0.021, 0.016) * u_time_s + vec2(17.3, -9.1));
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
	float foam = max(max(shorewash, whitecap * 0.78), max(breaker_foam, diffracted_foam));
	foam = max(foam, v_interaction_foam);
	foam = clamp(foam * u_foam_quality * (1.0 - smoothstep(3500.0, 18000.0, length(VERTEX))), 0.0, 1.0);
	base = mix(base, u_foam, foam * 0.88);

	float wind_rough = water_roughness_from_wind(wind_speed, 0.5);
	float all_unresolved_variance = max(residual_unresolved_variance, 0.0)
		+ max(micro_unresolved_variance, 0.0);
	float unresolved_slope_rms = sqrt(all_unresolved_variance);
	float wind_alpha = wind_rough * wind_rough;
	float spectral_alpha = clamp(unresolved_slope_rms * 0.82, 0.0, 0.24);
	float rough = sqrt(clamp(wind_alpha + spectral_alpha, 0.004, 0.44));
	rough = clamp(rough + foam * 0.18, 0.060, 0.76);

	vec3 sun_dir = normalize(u_sun_dir);
	vec3 view_dir = normalize(camera_planet - v_planet);
	float ndotv = max(dot(n, view_dir), 0.0);
	float fres = water_fresnel(ndotv);

	float backlight = pow(max(dot(-sun_dir, view_dir), 0.0), 4.0);
	float crest_thin = smoothstep(0.55, 0.96, v_crest) * (1.0 - smoothstep(2.0, 18.0, depth));
	vec3 crest_scatter = u_scatter * backlight * crest_thin * u_crest_scatter_strength;

	vec3 atmo_t;
	vec3 atmo_l;
	aerial_perspective(camera_planet, v_planet, atmo_t, atmo_l);

	ALBEDO = base * (1.0 - fres * 0.72) * atmo_t;
	EMISSION = crest_scatter * atmo_t + atmo_l;
	NORMAL = normalize((VIEW_MATRIX * vec4(n, 0.0)).xyz);
	ROUGHNESS = rough;
	SPECULAR = 0.25;
	METALLIC = 0.0;
	AO = 1.0;
	AO_LIGHT_AFFECT = 1.0;
}

void light() {
	float visibility = 1.0;
	vec3 direct_t = vec3(1.0);
	if (LIGHT_IS_DIRECTIONAL) {
		visibility = planet_direct_sun_visibility(v_planet, normalize(u_sun_dir), u_planet_radius);
		visibility *= clamp(v_cloud_shadow, 0.0, 1.0);
		direct_t = clamp(v_direct_atmo_t, vec3(0.0), vec3(1.0));
	}

	float volume_diffuse = mix(0.30, 0.10, smoothstep(0.5, 35.0, v_depth));
	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	DIFFUSE_LIGHT += ndotl * ATTENUATION * LIGHT_COLOR * direct_t
		* visibility * volume_diffuse / PI;

	float spec = water_specular(NORMAL, VIEW, LIGHT, ROUGHNESS);
	float fres = water_fresnel(max(dot(NORMAL, VIEW), 0.0));
	SPECULAR_LIGHT += LIGHT_COLOR * direct_t * ATTENUATION * visibility
		* spec * fres * SPECULAR_AMOUNT;
}
