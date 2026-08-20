shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx;
/// Terrain surface.

instance uniform float morph : hint_range(0.0, 1.0) = 0.0;
instance uniform float chunk_depth = 0.0;

#include "res://shaders/atmosphere.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/coastline_clipmap.gdshaderinc"

uniform vec3 u_origin = vec3(0.0);
uniform sampler2DArray u_orbit_elevation;
uniform float u_orbit_face_res = 192.0;
uniform float u_orbit_coast_start_altitude = 12000.0;
uniform float u_debug_uv : hint_range(0.0, 1.0) = 0.0;
uniform float u_debug_lod : hint_range(0.0, 1.0) = 0.0;
uniform float u_debug_plain : hint_range(0.0, 1.0) = 0.0;
uniform float u_mottle : hint_range(0.0, 0.4) = 0.085;
uniform float u_rock_tint : hint_range(0.0, 1.0) = 0.55;
uniform vec3 u_rock_dark : source_color = vec3(0.30, 0.28, 0.26);
uniform vec3 u_rock_light : source_color = vec3(0.55, 0.52, 0.47);

varying vec3 v_planet;

void vertex() {
	VERTEX += CUSTOM0.xyz * morph;
	v_planet = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz + u_origin;
}

float hash13(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.yzx + 33.33);
	return fract((p.x + p.y) * p.z);
}

float vnoise(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(
		mix(mix(hash13(i), hash13(i + vec3(1.0, 0.0, 0.0)), f.x),
			mix(hash13(i + vec3(0.0, 1.0, 0.0)), hash13(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
		mix(mix(hash13(i + vec3(0.0, 0.0, 1.0)), hash13(i + vec3(1.0, 0.0, 1.0)), f.x),
			mix(hash13(i + vec3(0.0, 1.0, 1.0)), hash13(i + vec3(1.0, 1.0, 1.0)), f.x), f.y),
		f.z);
}

float uv_glyph(vec2 uv) {
	vec2 p = uv - vec2(0.5);
	if (abs(p.x) < 0.045 && p.y > -0.28 && p.y < 0.16) return 1.0;
	float head = (p.y - 0.16) / 0.24;
	if (head >= 0.0 && head <= 1.0 && abs(p.x) < 0.15 * (1.0 - head)) return 1.0;
	if (abs(p.y) < 0.03 && p.x > 0.16 && p.x < 0.34) return 1.0;
	float tip = (p.x - 0.34) / 0.10;
	if (tip >= 0.0 && tip <= 1.0 && abs(p.y) < 0.06 * (1.0 - tip)) return 1.0;
	return 0.0;
}

vec3 depth_color(float d) {
	return 0.5 + 0.5 * cos(TAU * (d * 0.13 + vec3(0.0, 0.33, 0.67)));
}

void fragment() {
	vec3 up = normalize(v_planet);
	vec3 camera_planet = CAMERA_POSITION_WORLD + u_origin;
	float camera_alt = length(camera_planet) - u_planet_radius;

	if (camera_alt >= u_orbit_coast_start_altitude) {
		float visual_h = coast_surface_height(u_orbit_elevation, up, u_orbit_face_res, u_planet_radius);
		if (visual_h < 0.0) discard;
	}

	vec3 wn = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
	float slope = clamp(1.0 - dot(wn, up), 0.0, 1.0);
	float dist = length(VERTEX);
	vec3 pd = up * u_planet_radius;

	float k_mid = 1.0 - smoothstep(3000.0, 26000.0, dist);
	float k_fine = 1.0 - smoothstep(150.0, 1100.0, dist);
	float k_near = 1.0 - smoothstep(18.0, 120.0, dist);
	float mottle = (vnoise(pd * 0.0016) - 0.5) * k_mid;
	if (k_fine > 0.002) mottle += (vnoise(pd * 0.021) - 0.5) * 0.7 * k_fine;
	if (k_near > 0.002) mottle += (vnoise(pd * 0.35) - 0.5) * 0.9 * k_near;

	vec3 albedo = COLOR.rgb * (1.0 + mottle * u_mottle * 2.0);
	float rock = smoothstep(0.10, 0.42, slope);
	vec3 rock_col = mix(u_rock_dark, u_rock_light, vnoise(pd * 0.008));
	albedo = mix(albedo, rock_col, rock * u_rock_tint);

	albedo = mix(albedo, vec3(0.34, 0.35, 0.37), u_debug_plain);
	if (u_debug_lod > 0.0) albedo = mix(albedo, depth_color(chunk_depth), u_debug_lod * 0.5);
	if (u_debug_uv > 0.0) {
		float edge = min(min(UV.x, 1.0 - UV.x), min(UV.y, 1.0 - UV.y));
		float border = 1.0 - smoothstep(0.004, 0.010, edge);
		albedo = mix(albedo, vec3(0.02, 0.02, 0.03), border * u_debug_uv);
		albedo = mix(albedo, depth_color(chunk_depth) * 0.30, uv_glyph(UV) * u_debug_uv);
	}

	// Keep material colour physically stable. Cloud cover now attenuates only the
	// directional light in light(), leaving sky/ambient illumination intact.
	ALBEDO = albedo;
	ROUGHNESS = mix(0.94, 0.82, rock);
	SPECULAR = 0.12;
	METALLIC = 0.0;
	FOG = aerial_fog(camera_planet, v_planet);
}

void light() {
	float visibility = 1.0;
	if (LIGHT_IS_DIRECTIONAL) {
		visibility = cloud_surface_sun_visibility(v_planet, normalize(u_sun_dir));
	}

	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	DIFFUSE_LIGHT += ndotl * ATTENUATION * LIGHT_COLOR * visibility / PI;

	// Terrain specular is deliberately subdued, but it must also disappear under
	// an opaque cloud deck instead of shining through the projected shadow.
	if (ndotl > 0.0) {
		vec3 h = normalize(LIGHT + VIEW);
		float ndoth = max(dot(NORMAL, h), 0.0);
		float spec_power = mix(96.0, 5.0, clamp(ROUGHNESS, 0.0, 1.0));
		float spec = pow(ndoth, spec_power) * (1.0 - ROUGHNESS) * 0.22;
		SPECULAR_LIGHT += LIGHT_COLOR * ATTENUATION * visibility * spec * SPECULAR_AMOUNT;
	}
}
