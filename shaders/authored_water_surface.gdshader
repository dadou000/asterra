shader_type spatial;
render_mode blend_mix, cull_disabled, depth_prepass_alpha, diffuse_burley, specular_schlick_ggx, ambient_light_disabled;

uniform float u_planet_radius = 1000000.0;

#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/gpu_terrain_height.gdshaderinc"
#include "res://shaders/terrain_edit_delta.gdshaderinc"
#include "res://shaders/water_surface.gdshaderinc"

uniform vec3 u_origin = vec3(0.0);
uniform float u_author_time_s = 0.0;
uniform int u_feature_type = 0; // 0 lake, 1 river
uniform float u_feature_wave_amplitude_scale = 1.0;
uniform float u_feature_wave_frequency_scale = 1.0;
uniform float u_global_wave_amplitude_scale = 1.0;
uniform float u_global_wave_frequency_scale = 1.0;
uniform float u_current_scale = 1.0;
uniform float u_shore_falloff_m = 3.0;
uniform float u_default_depth_m = 5.0;
uniform vec3 u_deep : source_color = vec3(0.010, 0.042, 0.105);
uniform vec3 u_shelf : source_color = vec3(0.058, 0.300, 0.360);
uniform vec3 u_shallow : source_color = vec3(0.130, 0.475, 0.440);
uniform vec3 u_foam : source_color = vec3(0.82, 0.90, 0.94);

varying vec3 v_planet;
varying float v_authored_depth;
varying float v_current_m_s;
varying float v_cross_section;
varying float v_wave_energy;

const vec3 AUTHORED_WAVE_AXIS_A = vec3(0.827, 0.201, 0.525);
const vec3 AUTHORED_WAVE_AXIS_B = vec3(-0.436, 0.331, 0.837);

vec3 authored_safe_tangent(vec3 up, vec3 axis) {
	vec3 t = axis - up * dot(axis, up);
	if (dot(t, t) < 1e-8) {
		vec3 fallback = abs(up.y) < 0.92 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		t = fallback - up * dot(fallback, up);
	}
	return normalize(t);
}

void vertex() {
	vec3 render_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	vec3 planet = render_world + u_origin;
	vec3 up = normalize(planet);

	v_authored_depth = max(UV.x, 0.0);
	v_current_m_s = max(UV.y, 0.0);
	v_cross_section = UV2.x;

	float feature_wave = max(u_feature_wave_amplitude_scale, 0.0)
		* max(u_global_wave_amplitude_scale, 0.0);
	float amplitude_m = 0.095 * feature_wave;
	if (u_feature_type == 1) {
		amplitude_m *= 0.24;
	}
	float frequency_scale = max(u_feature_wave_frequency_scale
		* u_global_wave_frequency_scale, 0.001);
	float wavelength_m = (u_feature_type == 1 ? 5.5 : 18.0) / frequency_scale;
	float k = TAU / max(wavelength_m, 0.25);
	float current_speed = max(v_current_m_s * u_current_scale, 0.0);
	float time_speed = (u_feature_type == 1 ? 1.8 + current_speed * 0.22 : 1.15);
	float phase_a = dot(planet, AUTHORED_WAVE_AXIS_A) * k + u_author_time_s * time_speed;
	float phase_b = dot(planet, AUTHORED_WAVE_AXIS_B) * (k * 1.73)
		- u_author_time_s * (time_speed * 0.73);
	float wave = (sin(phase_a) * 0.72 + sin(phase_b) * 0.28) * amplitude_m;

	vec3 tangent_a = authored_safe_tangent(up, AUTHORED_WAVE_AXIS_A);
	vec3 tangent_b = authored_safe_tangent(up, AUTHORED_WAVE_AXIS_B);
	float slope_a = cos(phase_a) * amplitude_m * k * 0.72;
	float slope_b = cos(phase_b) * amplitude_m * (k * 1.73) * 0.28;
	vec3 displaced_normal = normalize(up - tangent_a * slope_a - tangent_b * slope_b);

	VERTEX += NORMAL * wave;
	NORMAL = displaced_normal;
	v_planet = planet + up * wave;
	v_wave_energy = clamp(abs(slope_a) + abs(slope_b), 0.0, 1.0);
}

void fragment() {
	vec3 dir = normalize(v_planet);
	float water_height_m = length(v_planet) - u_planet_radius;
	float terrain_height_m = gpu_terrain_height_for_level(dir, 0)
		+ terrain_edit_delta(dir, u_planet_radius);
	float geometric_depth_m = water_height_m - terrain_height_m;

	// Authored water remains non-destructive: it never cuts the terrain mesh.
	// Instead the surface is clipped when the current terrain rises through it.
	// A few centimetres of tolerance avoids z-fighting where a river was clicked
	// exactly on the rendered ground surface.
	if (u_macro_ready > 0.5 && geometric_depth_m < -0.08) {
		discard;
	}
	float depth_m = u_macro_ready > 0.5
		? max(geometric_depth_m, 0.0)
		: max(v_authored_depth, u_default_depth_m);

	vec3 base = water_depth_tint(depth_m, u_shallow, u_shelf, u_deep);
	float shallow_scale = clamp(u_shore_falloff_m * 0.08, 0.08, 4.0);
	float shore = smoothstep(0.0, shallow_scale, depth_m);
	float foam = (1.0 - smoothstep(0.03, 0.52, depth_m))
		* smoothstep(0.02, 0.18, depth_m);
	base = mix(base, u_foam, foam * (0.20 + v_wave_energy * 0.32));

	float edge_alpha = 1.0;
	if (u_feature_type == 1) {
		edge_alpha = 1.0 - smoothstep(0.82, 1.0, abs(v_cross_section));
	}
	float alpha = mix(0.52, 0.88, shore) * edge_alpha;
	if (alpha <= 0.01) {
		discard;
	}

	ALBEDO = base;
	ROUGHNESS = clamp(0.055 + v_wave_energy * 0.18, 0.045, 0.28);
	SPECULAR = 0.58;
	METALLIC = 0.0;
	ALPHA = alpha;
}
