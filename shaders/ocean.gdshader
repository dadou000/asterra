shader_type spatial;
render_mode blend_mix, cull_disabled, depth_draw_always, diffuse_burley, specular_schlick_ggx, ambient_light_disabled;
/// Free water: oceans and lakes.

instance uniform float morph : hint_range(0.0, 1.0) = 0.0;

#include "res://shaders/atmosphere.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/coastline_clipmap.gdshaderinc"
#include "res://shaders/water_surface.gdshaderinc"

uniform vec3 u_origin = vec3(0.0);
uniform sampler2DArray u_orbit_elevation;
uniform float u_orbit_face_res = 192.0;
uniform vec3 u_deep : source_color = vec3(0.010, 0.042, 0.105);
uniform vec3 u_shelf : source_color = vec3(0.058, 0.300, 0.360);
uniform vec3 u_shallow : source_color = vec3(0.130, 0.475, 0.440);
uniform vec3 u_foam : source_color = vec3(0.82, 0.90, 0.94);
uniform float u_wave_scale : hint_range(0.0, 3.0) = 1.0;
uniform float u_depth_scale = 4000.0;
uniform float u_orbit_handoff_altitude = 1000000.0;
uniform float u_shore_blur : hint_range(0.0, 3.0) = 0.35;
uniform float u_cell_per_metre = 0.0223;
/// Range over which the distant shoreline stops trusting the water mesh's own
/// vertices and takes the whole-planet height texture instead. The near end is
/// past the horizon of any view from under a kilometre up.
const float COAST_MESH_FADE_NEAR_M = 60000.0;
const float COAST_MESH_FADE_FAR_M = 130000.0;

varying vec3 v_planet;
/// Aerial transmittance, written in fragment() so light() can attenuate the sun
/// glint it adds. See aerial_perspective in atmosphere.gdshaderinc.
varying vec3 v_atmo_t;
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
	// Past the clipmap the mesh is the *coarse* representation of the shoreline,
	// not the fine one. A water chunk that far out carries one depth sample every
	// few tens of kilometres, so the zero crossing of its interpolated vertex
	// depth is literally a polyline on the triangles: the coast arrives as a
	// chain of straight runs metering tens of kilometres, meeting at sharp
	// corners. It is the most visible single artifact in any distant view, and no
	// amount of shading work hides a continent with a polygon for an outline.
	// The whole-planet texture is ~2 km per texel and smooth, so out there it is
	// strictly the better source; hand back to it as the mesh loses resolution.
	//
	// Ramped on distance, never on chunk depth: depth steps by one across a
	// quadtree boundary and any weight derived from it stamps that step into the
	// coastline. The near end sits beyond the horizon of any low-altitude view,
	// so walking the shore still uses the clipmap and the mesh, as before.
	float far_w = smoothstep(COAST_MESH_FADE_NEAR_M, COAST_MESH_FADE_FAR_M, dist);
	float ocean_ground_h = mix(-v_signed_depth, visual_h, max(clip_weight, far_w));
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
	vec3 base = water_depth_tint(depth, u_shallow, u_shelf, u_deep);
	float crest = clamp(h / max(0.15, 0.9 * a) * 0.5 + 0.5, 0.0, 1.0);
	float foam = smoothstep(1.6, 0.0, depth) * smoothstep(0.35, 0.85, crest) * (1.0 - smoothstep(1500.0, 12000.0, dist));
	base = mix(base, u_foam, foam * 0.75);

	vec3 sun_dir = normalize(u_sun_dir);
	vec3 skylight = planet_sky_irradiance(v_planet, sun_dir, u_planet_radius);

	// Sea state from the generator's own wind bands. The wave geometry above is
	// a fixed swell; this is the sub-wavelength chop that actually decides how
	// wide the sun's reflection spreads.
	float gust = surface_vnoise(ps * 0.000032 + vec3(5.0, -17.0, 26.0));
	float rough = water_roughness_from_wind(water_wind_speed(up), gust);
	// Wave slope and foam both scatter further than flat water does.
	rough = clamp(rough + clamp(length(slope) * 0.20, 0.0, 0.10) + foam * 0.45, 0.02, 1.0);

	vec3 view_dir = normalize(camera_planet - v_planet);
	float ndotv = max(dot(n, view_dir), 0.0);
	float fres = water_fresnel(ndotv);
	vec3 sky_refl = water_sky_radiance(reflect(-view_dir, n), up, sun_dir);

	vec3 atmo_t;
	vec3 atmo_l;
	aerial_perspective(camera_planet, v_planet, atmo_t, atmo_l);
	v_atmo_t = atmo_t;
	ALBEDO = base * (1.0 - fres * 0.85) * atmo_t;
	EMISSION = (base * skylight + sky_refl * fres * 0.9) * atmo_t + atmo_l;
	NORMAL = normalize((VIEW_MATRIX * vec4(n, 0.0)).xyz);
	ROUGHNESS = rough;
	SPECULAR = 0.5;
	METALLIC = 0.0;
	AO = 1.0;
	AO_LIGHT_AFFECT = 1.0;

	float shore;
	if (v_ocean > 0.5) {
		float coast_aa = max(fwidth(ocean_ground_h) * 1.15, 0.35);
		shore = 1.0 - smoothstep(-coast_aa, coast_aa, ocean_ground_h);
	} else {
		// Screen footprint in metres, taken from the view-space position rather
		// than the planet-space one. `v_planet` is a float32 holding about 1e6, so
		// it quantises to a few centimetres and its screen derivative is mostly
		// quantisation noise -- which arrived as a dotted black rim around every
		// lake and river instead of a shoreline. VERTEX is small and exact.
		float metres_per_px = max(length(fwidth(VERTEX)), 1e-3);
		float depth_slope = max(fwidth(v_signed_depth) / metres_per_px, 1e-6);
		float to_shore = v_signed_depth / depth_slope;
		float cell = dist * u_cell_per_metre;
		float aa = max(metres_per_px * 1.25, 0.30);
		float blur = max(cell * u_shore_blur, aa);
		shore = smoothstep(-aa, blur, to_shore) * v_mask;
		if (shore < 0.001) discard;
	}

	ALPHA = shore * mix(0.62, 1.0, smoothstep(0.0, 7.0, depth));
}

void light() {
	float visibility = 1.0;
	if (LIGHT_IS_DIRECTIONAL) {
		vec3 sun_dir = normalize(u_sun_dir);
		visibility = planet_direct_sun_visibility(v_planet, sun_dir, u_planet_radius);
	}

	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	DIFFUSE_LIGHT += ndotl * ATTENUATION * LIGHT_COLOR * visibility / PI;

	// Sun glint. GGX rather than a Phong lobe, so the highlight elongates into a
	// glitter path along the wind rather than sitting as a soft circular blob.
	float spec = water_specular(NORMAL, VIEW, LIGHT, ROUGHNESS);
	float fres = water_fresnel(max(dot(NORMAL, VIEW), 0.0));
	SPECULAR_LIGHT += LIGHT_COLOR * ATTENUATION * visibility * spec * fres
		* SPECULAR_AMOUNT * v_atmo_t;
}
