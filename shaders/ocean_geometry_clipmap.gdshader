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
uniform float u_grid_cells = 128.0;
uniform float u_visible_cap_angle = 1.5707963;
uniform float u_orbit_handoff_altitude = 120000.0;
uniform float u_wave_scale : hint_range(0.0, 3.0) = 1.0;
uniform vec3 u_deep : source_color = vec3(0.010, 0.042, 0.105);
uniform vec3 u_shelf : source_color = vec3(0.058, 0.300, 0.360);
uniform vec3 u_shallow : source_color = vec3(0.130, 0.475, 0.440);
uniform vec3 u_foam : source_color = vec3(0.88, 0.93, 0.96);

varying vec3 v_planet;
varying vec3 v_render;
varying vec3 v_slope;
varying float v_depth;
varying float v_valid;
varying float v_breaking;
varying float v_crest;
varying float v_compression;
varying float v_diffraction;
varying float v_level;
varying float v_cloud_shadow;

const float MAX_PROJECTED_THETA = 1.62;
const float TANGENT_APPROX_MAX_ARC_M = 90000.0;
const float FINE_MORPH_START_Q = 0.875;
const float RING_INNER_Q = 0.4375;
const float RING_HANDOFF_Q = 0.50;
const float TERRAIN_HALF_CELLS = 200.0;
const float TERRAIN_FINE_MORPH_START_Q = 0.875;

// Wave dynamics use one fixed full-detail bathymetry field at every ocean LOD.
// Level 2 still carries every current terrain detail band (including 24 m) but
// cannot change when the render clipmap changes level. The finite-difference
// footprint is also fixed, so the same planet point always gets the same depth,
// shore distance, refraction direction and dispersion frequency.
const int OCEAN_BATHY_LEVEL = 2;
const float OCEAN_BATHY_SAMPLE_M = 56.0;

const vec3 SWELL_AXIS_A = vec3(0.827, 0.201, 0.525);
const vec3 SWELL_AXIS_B = vec3(-0.436, 0.331, 0.837);

float ocean_ring_spacing(int level) {
	return u_base_spacing * exp2(float(level));
}

float ocean_smoother01(float x) {
	x = clamp(x, 0.0, 1.0);
	return x * x * x * (x * (x * 6.0 - 15.0) + 10.0);
}

float ocean_fine_morph(vec2 cell) {
	float q = length(cell) / max(u_grid_cells * 0.5, 1.0);
	return ocean_smoother01((q - FINE_MORPH_START_Q) / (1.0 - FINE_MORPH_START_Q));
}

float ocean_coarse_sink(vec2 cell, int level, float spacing) {
	if (level <= 0) return 0.0;
	float q = length(cell) / max(u_grid_cells * 0.5, 1.0);
	float blend = 1.0 - ocean_smoother01((q - RING_INNER_Q) / (RING_HANDOFF_Q - RING_INNER_Q));
	return blend * clamp(spacing * 0.16, 0.05, 3.0);
}

vec3 ocean_direction_for_offset(vec2 offset_m) {
	float arc = length(offset_m);
	if (arc < 1e-5) return normalize(u_center_dir);
	// Match spherical_geometry_clipmap_procedural_uv.gdshader exactly: the active
	// terrain uses tangent-plane projection through 90 km, independent of LOD.
	if (arc <= TANGENT_APPROX_MAX_ARC_M) {
		return normalize(u_center_dir
			+ u_center_right * (offset_m.x / u_planet_radius)
			+ u_center_up * (offset_m.y / u_planet_radius));
	}
	float theta = min(arc / max(u_planet_radius, 1.0), min(u_visible_cap_angle * 1.02, MAX_PROJECTED_THETA));
	vec3 tangent = normalize(u_center_right * offset_m.x + u_center_up * offset_m.y);
	return normalize(u_center_dir * cos(theta) + tangent * sin(theta));
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

void ocean_bathymetry(vec3 d, float arc_m,
		out float raw_ground_h, out float smooth_ground_h,
		out vec3 landward, out float signed_shore_distance,
		out float coast_confidence) {
	// The exact visible terrain LOD owns only the wet/dry mask. This preserves a
	// coastline that is pixel-consistent with the current terrain representation.
	int terrain_level;
	float terrain_morph;
	ocean_visible_terrain_lod(arc_m, terrain_level, terrain_morph);
	raw_ground_h = gpu_terrain_height_with_morph(d, terrain_level, terrain_morph);

	// Everything that controls the physical wave field comes from a fixed terrain
	// evaluator. It is therefore identical for coincident vertices in L0, L1, ...
	float physical_ground_h = gpu_terrain_height_for_level(d, OCEAN_BATHY_LEVEL);
	smooth_ground_h = physical_ground_h;
	landward = ocean_safe_tangent(d, SWELL_AXIS_A);
	signed_shore_distance = -max(-physical_ground_h, 0.0) * 8.0;
	coast_confidence = 0.0;

	if (abs(physical_ground_h) > 320.0) return;

	vec3 tx = ocean_safe_tangent(d, u_center_right);
	vec3 ty = ocean_safe_tangent(d, u_center_up);
	float s = OCEAN_BATHY_SAMPLE_M;
	float hx0 = gpu_terrain_height_for_level(ocean_offset_dir(d, tx, -s), OCEAN_BATHY_LEVEL);
	float hx1 = gpu_terrain_height_for_level(ocean_offset_dir(d, tx,  s), OCEAN_BATHY_LEVEL);
	float hy0 = gpu_terrain_height_for_level(ocean_offset_dir(d, ty, -s), OCEAN_BATHY_LEVEL);
	float hy1 = gpu_terrain_height_for_level(ocean_offset_dir(d, ty,  s), OCEAN_BATHY_LEVEL);

	// Broad enough to reject 24 m terrain microstructure while preserving beaches,
	// points and bays for refraction/diffraction.
	smooth_ground_h = (physical_ground_h * 4.0 + hx0 + hx1 + hy0 + hy1) * 0.125;
	vec2 g = vec2(hx1 - hx0, hy1 - hy0) / max(2.0 * s, 1.0);
	float gm = length(g);
	coast_confidence = smoothstep(0.025, 0.18, gm);
	landward = gm > 1e-4 ? normalize(tx * g.x + ty * g.y) : landward;
	signed_shore_distance = clamp(smooth_ground_h / max(gm, 0.025), -6000.0, 6000.0);
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

	// This is now the only LOD-dependent input to the wave solver. It controls
	// representability of short wavelengths, not physical phase/direction/time.
	// At the fine ring's outer edge it becomes exactly the parent spacing, which
	// matches the coarse ring at the same planet-space points.
	float morph = ocean_fine_morph(cell);
	float wave_spacing = mix(spacing, spacing * 2.0, morph);

	float raw_ground_h;
	float ground_h;
	vec3 landward;
	float shore_distance;
	float coast_confidence;
	ocean_bathymetry(d, arc_m,
		raw_ground_h, ground_h, landward, shore_distance, coast_confidence);
	float depth = max(-ground_h, 0.0);

	vec3 p0 = d * u_planet_radius;
	vec3 swell_a = ocean_safe_tangent(d, SWELL_AXIS_A);
	vec3 swell_b = ocean_safe_tangent(d, SWELL_AXIS_B);
	float shallow_refract = 1.0 - smoothstep(22.0, 150.0, depth);
	float refract = shallow_refract * coast_confidence;
	vec3 refracted_a = normalize(mix(swell_a, landward, refract));
	vec3 refracted_b = normalize(mix(swell_b, landward, refract * 0.88));
	float diffract_a = ocean_diffraction_weight(d, swell_a, landward, depth) * coast_confidence;
	float diffract_b = ocean_diffraction_weight(d, swell_b, landward, depth) * coast_confidence * 0.82;
	vec3 dir_a = normalize(mix(refracted_a, ocean_diffraction_direction(d, swell_a, landward), diffract_a * 0.52));
	vec3 dir_b = normalize(mix(refracted_b, ocean_diffraction_direction(d, swell_b, landward), diffract_b * 0.46));

	// Planet-space phase axes: no clipmap coordinates participate here.
	float phase_a = dot(p0, normalize(SWELL_AXIS_A));
	float phase_b = dot(p0, normalize(SWELL_AXIS_B));

	OceanWaveSample wave = ocean_empty_wave();
	float t = TIME;
	if (raw_ground_h < 0.45) {
		ocean_add_wave(d, dir_a, phase_a, shore_distance, depth, wave_spacing, 96.0, 0.78 * u_wave_scale, 0.62, t, 0.0, wave);
		ocean_add_wave(d, dir_b, phase_b, shore_distance, depth, wave_spacing, 42.0, 0.32 * u_wave_scale, 0.55, t, 1.7, wave);
		ocean_add_wave(d, dir_a, phase_a, shore_distance, depth, wave_spacing, 18.0, 0.105 * u_wave_scale, 0.48, t, 3.1, wave);
		ocean_add_wave(d, dir_b, phase_b, shore_distance, depth, wave_spacing, 8.0, 0.036 * u_wave_scale, 0.42, t, 4.6, wave);
	}

	float sink = ocean_coarse_sink(cell, level, spacing);
	vec3 displaced = p0 + wave.displacement - d * sink;
	v_planet = displaced;
	v_render = displaced - u_origin;
	v_slope = wave.slope;
	v_depth = depth;
	v_breaking = wave.breaking;
	v_crest = wave.crest;
	v_compression = wave.compression;
	v_diffraction = max(diffract_a, diffract_b);
	v_level = float(level);

	vec3 shadow_sun = normalize(u_cloud_shadow_sun_dir);
	float planet_vis = planet_direct_sun_visibility(displaced, shadow_sun, u_planet_radius);
	v_cloud_shadow = planet_vis > 0.001
		? cloud_shadow_transmittance(displaced, shadow_sun, u_planet_radius)
		: 0.0;

	v_valid = (u_macro_ready > 0.5 && raw_ground_h < 0.45 && raw_theta <= u_visible_cap_angle) ? 1.0 : 0.0;
	VERTEX = v_render;
}

void fragment() {
	if (v_valid < 0.5) discard;
	vec3 camera_planet = CAMERA_POSITION_WORLD + u_origin;
	float camera_alt = length(camera_planet) - u_planet_radius;
	if (camera_alt >= u_orbit_handoff_altitude) discard;

	vec3 up = normalize(v_planet);
	vec3 n = normalize(up - v_slope);
	if (dot(n, up) < 0.0) n = -n;

	float depth = v_depth;
	vec3 base = water_depth_tint(depth, u_shallow, u_shelf, u_deep);
	float surf_zone = 1.0 - smoothstep(0.2, 9.0, depth);
	float shorewash = (1.0 - smoothstep(0.12, 3.8, depth)) * smoothstep(0.50, 0.93, v_crest);
	float whitecap = v_compression * smoothstep(0.54, 0.90, v_crest);
	float breaker_foam = v_breaking * (0.68 + 0.32 * v_compression);
	float diffracted_foam = v_diffraction * surf_zone * v_compression * 0.35;
	float foam = max(max(shorewash, whitecap * 0.55), max(breaker_foam, diffracted_foam));
	foam = clamp(foam * (1.0 - smoothstep(2500.0, 14000.0, length(VERTEX))), 0.0, 1.0);
	base = mix(base, u_foam, foam * 0.90);

	vec3 sun_dir = normalize(u_sun_dir);
	vec3 skylight = planet_sky_irradiance(v_planet, sun_dir, u_planet_radius);
	float rough = water_roughness_from_wind(water_wind_speed(up), 0.5);
	rough = clamp(rough + foam * 0.50, 0.025, 0.72);

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
