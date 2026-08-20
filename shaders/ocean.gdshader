shader_type spatial;
render_mode blend_mix, cull_disabled, depth_draw_always, diffuse_burley, specular_schlick_ggx;
/// Free water: oceans and lakes.

instance uniform float morph : hint_range(0.0, 1.0) = 0.0;

#include "res://shaders/atmosphere.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/coastline_clipmap.gdshaderinc"

uniform vec3 u_origin = vec3(0.0);
uniform sampler2DArray u_orbit_elevation;
uniform float u_orbit_face_res = 192.0;
uniform vec3 u_deep : source_color = vec3(0.022, 0.082, 0.165);
uniform vec3 u_shallow : source_color = vec3(0.090, 0.395, 0.445);
uniform vec3 u_foam : source_color = vec3(0.82, 0.90, 0.94);
uniform float u_wave_scale : hint_range(0.0, 3.0) = 1.0;
uniform float u_depth_scale = 4000.0;
uniform float u_deep_at = 42.0;
uniform float u_orbit_handoff_altitude = 12000.0;
uniform float u_shore_blur : hint_range(0.0, 3.0) = 0.35;
uniform float u_cell_per_metre = 0.0223;

varying vec3 v_planet;
varying float v_signed_depth;
varying float v_mask;
varying float v_ocean;

void vertex() {
	VERTEX += CUSTOM0.xyz * morph;
	v_planet = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz + u_origin;
	float r2 = COLOR.r * COLOR.r;
	float depth_mag = r2 * r2 * u_depth_scale;
	v_signed_depth = depth_mag * (COLOR.a >= 0.5 ? 1.0 : -1.0);
	v_mask = COLOR.g;
	v_ocean = COLOR.b;
}

float octave(float wavelength, float dist) {
	return 1.0 - smoothstep(wavelength * 45.0, wavelength * 190.0, dist);
}

void wave(vec3 ps, vec3 up, vec3 dir, vec3 cross_dir, float k, float amp, float w, float t,
		inout float h, inout vec3 grad) {
	if (amp <= 0.0) return;
	float warp = sin(dot(ps, cross_dir) * k * 0.21 + t * w * 0.23);
	float ph = dot(ps, dir) * k + t * w + warp * 1.7;
	h += amp * sin(ph);
	grad += (dir - up * dot(dir, up)) * (amp * k * cos(ph));
}

void fragment() {
	vec3 camera_planet = CAMERA_POSITION_WORLD + u_origin;
	float camera_alt = length(camera_planet) - u_planet_radius;
	if (camera_alt >= u_orbit_handoff_altitude && v_ocean > 0.5) discard;

	vec3 up = normalize(v_planet);
	float dist = length(VERTEX);
	float t = TIME;
	float visual_h = coast_surface_height(u_orbit_elevation, up, u_orbit_face_res, u_planet_radius);
	float clip_weight = coast_clipmap_coverage(up, u_planet_radius);
	float ocean_ground_h = mix(-v_signed_depth, visual_h, clip_weight);
	float depth = v_ocean > 0.5 ? max(-ocean_ground_h, 0.0) : max(v_signed_depth, 0.0);

	if (v_ocean > 0.5) {
		float dry_aa = max(fwidth(ocean_ground_h) * 1.15, 0.35);
		if (ocean_ground_h > dry_aa * 1.5) discard;
	}

	vec3 ps = v_planet * (u_planet_radius / max(1.0, length(v_planet)));
	float shoaling = clamp(depth / 6.0, 0.06, 1.0);
	float a = u_wave_scale * shoaling;
	float h = 0.0;
	vec3 grad = vec3(0.0);
	wave(ps, up, vec3(0.827, 0.201, 0.525), vec3(-0.436, 0.331, 0.837), 0.0349, 0.90 * a * octave(180.0, dist), 0.585, t, h, grad);
	wave(ps, up, vec3(-0.436, 0.331, 0.837), vec3(0.827, 0.201, 0.525), 0.0660, 0.42 * a * octave(95.0, dist), 0.805, t, h, grad);
	wave(ps, up, vec3(0.581, -0.742, 0.334), vec3(-0.703, -0.264, 0.660), 0.2400, 0.13 * a * octave(26.0, dist), 1.530, t, h, grad);
	wave(ps, up, vec3(-0.703, -0.264, 0.660), vec3(0.581, -0.742, 0.334), 0.5700, 0.055 * a * octave(11.0, dist), 2.360, t, h, grad);
	wave(ps, up, vec3(0.246, 0.913, -0.325), vec3(-0.912, 0.128, -0.390), 1.8500, 0.018 * a * octave(3.4, dist), 4.260, t, h, grad);
	wave(ps, up, vec3(-0.912, 0.128, -0.390), vec3(0.246, 0.913, -0.325), 4.8000, 0.006 * a * octave(1.3, dist), 6.860, t, h, grad);

	vec3 slope = grad - up * dot(grad, up);
	vec3 n = normalize(up - slope);
	if (!FRONT_FACING) n = -n;
	float depth_t = clamp(depth / u_deep_at, 0.0, 1.0);
	vec3 base = mix(u_shallow, u_deep, depth_t * (2.0 - depth_t));
	float crest = clamp(h / max(0.15, 0.9 * a) * 0.5 + 0.5, 0.0, 1.0);
	float foam = smoothstep(1.6, 0.0, depth) * smoothstep(0.35, 0.85, crest) * (1.0 - smoothstep(1500.0, 12000.0, dist));
	base = mix(base, u_foam, foam * 0.75);

	ALBEDO = base;
	NORMAL = normalize((VIEW_MATRIX * vec4(n, 0.0)).xyz);
	ROUGHNESS = mix(0.04, 0.13, smoothstep(1500.0, 60000.0, dist)) + clamp(length(slope) * 0.22, 0.0, 0.12) + foam * 0.45;
	SPECULAR = 0.5;
	METALLIC = 0.0;

	float shore;
	if (v_ocean > 0.5) {
		float coast_aa = max(fwidth(ocean_ground_h) * 1.15, 0.35);
		shore = 1.0 - smoothstep(-coast_aa, coast_aa, ocean_ground_h);
	} else {
		float metres_per_px = max(length(fwidth(v_planet)), 1e-3);
		float depth_slope = max(fwidth(v_signed_depth) / metres_per_px, 1e-6);
		float to_shore = v_signed_depth / depth_slope;
		float cell = dist * u_cell_per_metre;
		float aa = max(metres_per_px * 1.25, 0.30);
		float blur = max(cell * u_shore_blur, aa);
		shore = smoothstep(-aa, blur, to_shore) * v_mask;
		if (shore < 0.001) discard;
	}

	ALPHA = shore * mix(0.62, 1.0, smoothstep(0.0, 7.0, depth));
	FOG = aerial_fog(camera_planet, v_planet);
}

void light() {
	float visibility = 1.0;
	if (LIGHT_IS_DIRECTIONAL) {
		visibility = cloud_surface_sun_visibility(v_planet, normalize(u_sun_dir));
	}

	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	DIFFUSE_LIGHT += ndotl * ATTENUATION * LIGHT_COLOR * visibility / PI;

	if (ndotl > 0.0) {
		vec3 h = normalize(LIGHT + VIEW);
		float ndoth = max(dot(NORMAL, h), 0.0);
		float ndotv = max(dot(NORMAL, VIEW), 0.0);
		float fresnel = 0.02 + 0.98 * pow(1.0 - ndotv, 5.0);
		float spec_power = mix(900.0, 18.0, clamp(ROUGHNESS, 0.0, 1.0));
		float spec = pow(ndoth, spec_power) * mix(0.18, 1.0, fresnel);
		SPECULAR_LIGHT += LIGHT_COLOR * ATTENUATION * visibility * spec * SPECULAR_AMOUNT * 1.8;
	}
}
