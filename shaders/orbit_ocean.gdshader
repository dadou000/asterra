shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx;
/// Orbit-only ocean shell.

#include "res://shaders/atmosphere.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/coastline_clipmap.gdshaderinc"

uniform vec3 u_origin = vec3(0.0);
uniform sampler2DArray u_orbit_elevation;
uniform float u_orbit_face_res = 192.0;
uniform float u_orbit_start_altitude = 30000.0;
uniform vec3 u_ocean_deep : source_color = vec3(0.018, 0.075, 0.145);
uniform vec3 u_ocean_mid : source_color = vec3(0.035, 0.175, 0.265);
uniform vec3 u_ocean_shelf : source_color = vec3(0.075, 0.340, 0.390);

varying vec3 v_planet;

void vertex() {
	v_planet = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz + u_origin;
}

void fragment() {
	vec3 camera_planet = CAMERA_POSITION_WORLD + u_origin;
	float camera_alt = length(camera_planet) - u_planet_radius;
	if (camera_alt < u_orbit_start_altitude) discard;

	vec3 up = normalize(v_planet);
	float visual_h = coast_surface_height(u_orbit_elevation, up, u_orbit_face_res, u_planet_radius);
	if (visual_h >= 0.0) discard;

	float depth = -visual_h;
	float mid_t = smoothstep(20.0, 550.0, depth);
	float deep_t = smoothstep(450.0, 3200.0, depth);
	vec3 base = mix(u_ocean_shelf, u_ocean_mid, mid_t);
	base = mix(base, u_ocean_deep, deep_t);
	vec3 p = up * u_planet_radius;
	float broad = surface_vnoise(p * 0.000018 + vec3(31.0, -12.0, 7.0)) - 0.5;
	base *= 1.0 + broad * 0.075;

	float cloud_visibility = cloud_surface_sun_visibility(v_planet, normalize(u_sun_dir));
	float cloud_shadow = 1.0 - cloud_visibility;
	base *= mix(vec3(1.0), vec3(0.72, 0.78, 0.86), cloud_shadow * 0.60);

	ALBEDO = base;
	NORMAL = normalize((VIEW_MATRIX * vec4(up, 0.0)).xyz);
	ROUGHNESS = mix(0.12, 0.055, deep_t);
	SPECULAR = 0.5;
	METALLIC = 0.0;
	AO = mix(1.0, 0.26, cloud_shadow);
	AO_LIGHT_AFFECT = 1.0;
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
		float spec_power = mix(420.0, 24.0, clamp(ROUGHNESS, 0.0, 1.0));
		float spec = pow(ndoth, spec_power) * mix(0.16, 1.0, fresnel);
		SPECULAR_LIGHT += LIGHT_COLOR * ATTENUATION * visibility * spec * SPECULAR_AMOUNT * 1.5;
	}
}
