shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx, ambient_light_disabled;
/// Orbit-only ocean shell.

#include "res://shaders/atmosphere.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/gpu_terrain_height.gdshaderinc"
#include "res://shaders/water_surface.gdshaderinc"

uniform vec3 u_origin = vec3(0.0);
uniform float u_orbit_start_altitude = 1000000.0;
uniform vec3 u_ocean_deep : source_color = vec3(0.008, 0.030, 0.075);
uniform vec3 u_ocean_mid : source_color = vec3(0.022, 0.125, 0.215);
uniform vec3 u_ocean_shelf : source_color = vec3(0.065, 0.330, 0.360);
uniform vec3 u_ocean_shoal : source_color = vec3(0.135, 0.470, 0.430);

varying vec3 v_planet;
varying vec3 v_atmo_t;

void vertex() {
	v_planet = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz + u_origin;
}

void fragment() {
	vec3 camera_planet = CAMERA_POSITION_WORLD + u_origin;
	float camera_alt = length(camera_planet) - u_planet_radius;
	if (camera_alt < u_orbit_start_altitude) discard;

	vec3 up = normalize(v_planet);
	// At orbital scale use L6 of the exact same procedural GPU field. L6 keeps
	// continental/coastal 480 m+ structure while intentionally filtering details
	// too small to resolve from >120 km altitude.
	float visual_h = gpu_terrain_height_for_level(up, 6);
	if (u_macro_ready < 0.5 || visual_h >= 0.0) discard;

	float depth = -visual_h;
	vec3 p = up * u_planet_radius;
	vec3 base = water_depth_tint(depth, u_ocean_shoal, u_ocean_shelf, u_ocean_deep);
	float broad = surface_vnoise(p * 0.000018 + vec3(31.0, -12.0, 7.0)) - 0.5;
	float plume = surface_vnoise(p * 0.000085 + vec3(-6.0, 22.0, 14.0)) - 0.5;
	base *= 1.0 + broad * 0.085 + plume * 0.05;
	float turbid = (1.0 - smoothstep(10.0, 260.0, depth)) * clamp(0.5 + plume, 0.0, 1.0);
	base = mix(base, u_ocean_shoal * 1.15, turbid * 0.28);

	vec3 sun_dir = normalize(u_sun_dir);
	vec3 skylight = planet_sky_irradiance(v_planet, sun_dir, u_planet_radius);
	float gust = surface_vnoise(p * 0.000032 + vec3(5.0, -17.0, 26.0));
	float wind = water_wind_speed(up);
	float rough = water_roughness_from_wind(wind, gust);

	vec3 view_dir = normalize(camera_planet - v_planet);
	float ndotv = max(dot(up, view_dir), 0.0);
	float fres = water_fresnel(ndotv);
	vec3 refl_dir = reflect(-view_dir, up);
	vec3 sky_refl = water_sky_radiance(refl_dir, up, sun_dir);

	vec3 atmo_t;
	vec3 atmo_l;
	aerial_perspective(camera_planet, v_planet, atmo_t, atmo_l);
	v_atmo_t = atmo_t;

	ALBEDO = base * (1.0 - fres * 0.85) * atmo_t;
	EMISSION = (base * skylight + sky_refl * fres * 0.9) * atmo_t + atmo_l;
	NORMAL = normalize((VIEW_MATRIX * vec4(up, 0.0)).xyz);
	ROUGHNESS = rough;
	SPECULAR = 0.5;
	METALLIC = 0.0;
	AO = 1.0;
	AO_LIGHT_AFFECT = 1.0;
}

void light() {
	float visibility = 1.0;
	if (LIGHT_IS_DIRECTIONAL) {
		vec3 sun_dir = normalize(u_sun_dir);
		visibility = planet_direct_sun_visibility(v_planet, sun_dir, u_planet_radius);
	}
	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	DIFFUSE_LIGHT += ndotl * ATTENUATION * LIGHT_COLOR * visibility / PI;
	float spec = water_specular(NORMAL, VIEW, LIGHT, ROUGHNESS);
	float fres = water_fresnel(max(dot(NORMAL, VIEW), 0.0));
	SPECULAR_LIGHT += LIGHT_COLOR * ATTENUATION * visibility * spec * fres
		* SPECULAR_AMOUNT * v_atmo_t;
}
