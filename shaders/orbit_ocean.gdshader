shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx, ambient_light_disabled;
/// Orbit-only ocean shell.
///
/// The local clipmap hands off to this representation at ~120 km. Even though
/// individual waves are no longer resolvable, the shell must preserve the same
/// air/water Fresnel, wind slope statistics and atmospheric extinction or the
/// handoff reads as a material change from orbit.

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

vec3 orbit_ocean_vertical_optical_depth() {
	return u_rayleigh_coeff * u_rayleigh_scale_height
		+ vec3(u_mie_coeff * u_mie_scale_height)
		+ u_ozone_coeff * u_ozone_width;
}

// Kasten-Young-style optical air mass for the incoming solar ray. At orbital
// range the old shell only attenuated light on the camera path, making sunrise
// and sunset glint far too white compared with the local ocean.
vec3 orbit_ocean_sun_transmittance(vec3 p, vec3 sun_dir) {
	float mu = dot(normalize(p), normalize(sun_dir));
	if (mu <= -0.015) return vec3(0.0);
	float m = max(mu, 0.001);
	float zenith_deg = degrees(acos(clamp(m, 0.0, 1.0)));
	float denom = m + 0.50572 * pow(max(96.07995 - zenith_deg, 0.1), -1.6364);
	float air_mass = min(1.0 / max(denom, 0.025), 38.0);
	return exp(-orbit_ocean_vertical_optical_depth() * air_mass);
}

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
	base *= 1.0 + broad * 0.055 + plume * 0.035;
	// The orbital shell has no seabed-albedo texture, so keep this intentionally
	// subtle. Strong turquoise continent-sized patches read as painted water.
	float turbid = (1.0 - smoothstep(4.0, 80.0, depth)) * clamp(0.5 + plume, 0.0, 1.0);
	base = mix(base, u_ocean_shoal * 1.08, turbid * 0.15);

	vec3 sun_dir = normalize(u_sun_dir);
	vec3 skylight = planet_sky_irradiance(v_planet, sun_dir, u_planet_radius);
	float gust = surface_vnoise(p * 0.000032 + vec3(5.0, -17.0, 26.0));
	float wind = water_wind_speed(up);
	float rough = water_roughness_from_wind(wind, gust);

	vec3 view_dir = normalize(camera_planet - v_planet);
	float ndotv = max(dot(up, view_dir), 0.0);
	float fres = water_fresnel(ndotv);
	vec3 refl_dir = reflect(-view_dir, up);
	// The orbit pass cannot reuse the camera-centred radiance cubemap: from above
	// the atmosphere that cubemap sees is not the sky seen by the water facet.
	// Retain the surface-centred analytic sky, but let rough water sample a broader
	// direction so it does not remain a mathematically sharp mirror at 120+ km.
	float sky_blur = clamp(rough * rough * 1.6, 0.0, 0.42);
	vec3 sky_sample_dir = normalize(mix(refl_dir, up, sky_blur));
	vec3 sky_refl = water_sky_radiance(sky_sample_dir, up, sun_dir);

	vec3 atmo_t;
	vec3 atmo_l;
	aerial_perspective(camera_planet, v_planet, atmo_t, atmo_l);
	v_atmo_t = atmo_t;

	// Reflection tends to 100% at grazing incidence, so body light tends to zero.
	// The former 15% diffuse floor made the planet-scale ocean look like blue paint.
	ALBEDO = base * (1.0 - fres) * atmo_t;
	EMISSION = (base * skylight * (1.0 - fres) + sky_refl * fres) * atmo_t + atmo_l;
	NORMAL = normalize((VIEW_MATRIX * vec4(up, 0.0)).xyz);
	ROUGHNESS = rough;
	SPECULAR = WATER_GODOT_SPECULAR;
	METALLIC = 0.0;
	AO = 1.0;
	AO_LIGHT_AFFECT = 1.0;
}

void light() {
	float visibility = 1.0;
	vec3 diffuse_t = vec3(1.0);
	vec3 specular_t = vec3(1.0);
	if (LIGHT_IS_DIRECTIONAL) {
		vec3 sun_dir = normalize(u_sun_dir);
		visibility = planet_direct_sun_visibility(v_planet, sun_dir, u_planet_radius);
		vec3 sun_t = orbit_ocean_sun_transmittance(v_planet, sun_dir);
		// ALBEDO already contains the surface->camera extinction. The diffuse light
		// therefore needs only the incoming solar path; custom specular bypasses
		// ALBEDO and needs both incoming and outgoing atmospheric paths.
		diffuse_t = sun_t;
		specular_t = sun_t * v_atmo_t;
	}

	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	DIFFUSE_LIGHT += ndotl * ATTENUATION * LIGHT_COLOR * visibility
		* diffuse_t / PI;

	vec3 wind_world = water_wind_direction(normalize(v_planet));
	vec3 wind_view = normalize((VIEW_MATRIX * vec4(wind_world, 0.0)).xyz);
	float stretch = water_cx_munk_stretch(water_wind_speed(normalize(v_planet)));
	float spec = water_specular_direct_anisotropic(
		NORMAL, wind_view, VIEW, LIGHT, ROUGHNESS, stretch);
	SPECULAR_LIGHT += LIGHT_COLOR * ATTENUATION * visibility * specular_t
		* spec * SPECULAR_AMOUNT;
}