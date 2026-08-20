shader_type spatial;
render_mode blend_mix, cull_disabled, depth_draw_always, diffuse_burley, specular_schlick_ggx;
/// Free water: oceans and lakes.
///
/// The streamed mesh only carries the water surface geometry. It no longer decides
/// the ocean coastline: sea-level fragments are clipped against the same planet-
/// space coastline clipmap used by terrain/orbit rendering. That removes the
/// stair-step shoreline caused by interpolating a 32x32 chunk's vertex depths.
/// Lakes keep the signed per-vertex depth path because each lake has its own level.
///
/// COLOR.b tags sea-level ocean vertices. Once the global ocean shell is active
/// those fragments are discarded, while elevated lake water remains rendered by
/// the streamed mesh.
///
/// COLOR.r stores the fourth-root magnitude of water-minus-ground height and
/// COLOR.a stores its sign for the lake/local-depth path.

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
	if (amp <= 0.0) {
		return;
	}
	float warp = sin(dot(ps, cross_dir) * k * 0.21 + t * w * 0.23);
	float ph = dot(ps, dir) * k + t * w + warp * 1.7;
	h += amp * sin(ph);
	grad += (dir - up * dot(dir, up)) * (amp * k * cos(ph));
}

void fragment() {
	vec3 camera_planet = CAMERA_POSITION_WORLD + u_origin;
	float camera_alt = length(camera_planet) - u_planet_radius;
	if (camera_alt >= u_orbit_handoff_altitude && v_ocean > 0.5) {
		discard;
	}

	vec3 up = normalize(v_planet);
	float dist = length(VERTEX);
	float t = TIME;

	// Sea-level ocean depth comes from the LOD-independent coastline field.
	// Lakes retain their own streamed signed depth because their free surface is
	// not sea level and is not represented by the global coast cache.
	float visual_h = coast_surface_height(
		u_orbit_elevation, up, u_orbit_face_res, u_planet_radius);
	float ocean_depth = max(-visual_h, 0.0);
	float depth = v_ocean > 0.5 ? ocean_depth : max(v_signed_depth, 0.0);

	vec3 ps = v_planet * (u_planet_radius / max(1.0, length(v_planet)));
	float shoaling = clamp(depth / 6.0, 0.06, 1.0);
	float a = u_wave_scale * shoaling;

	float h = 0.0;
	vec3 grad = vec3(0.0);
	wave(ps, up, vec3(0.827, 0.201, 0.525), vec3(-0.436, 0.331, 0.837),
		0.0349, 0.90 * a * octave(180.0, dist), 0.585, t, h, grad);
	wave(ps, up, vec3(-0.436, 0.331, 0.837), vec3(0.827, 0.201, 0.525),
		0.0660, 0.42 * a * octave(95.0, dist), 0.805, t, h, grad);
	wave(ps, up, vec3(0.581, -0.742, 0.334), vec3(-0.703, -0.264, 0.660),
		0.2400, 0.13 * a * octave(26.0, dist), 1.530, t, h, grad);
	wave(ps, up, vec3(-0.703, -0.264, 0.660), vec3(0.581, -0.742, 0.334),
		0.5700, 0.055 * a * octave(11.0, dist), 2.360, t, h, grad);
	wave(ps, up, vec3(0.246, 0.913, -0.325), vec3(-0.912, 0.128, -0.390),
		1.8500, 0.018 * a * octave(3.4, dist), 4.260, t, h, grad);
	wave(ps, up, vec3(-0.912, 0.128, -0.390), vec3(0.246, 0.913, -0.325),
		4.8000, 0.006 * a * octave(1.3, dist), 6.860, t, h, grad);

	vec3 slope = grad - up * dot(grad, up);
	vec3 n = normalize(up - slope);
	if (!FRONT_FACING) {
		n = -n;
	}

	float depth_t = clamp(depth / u_deep_at, 0.0, 1.0);
	vec3 base = mix(u_shallow, u_deep, depth_t * (2.0 - depth_t));

	float crest = clamp(h / max(0.15, 0.9 * a) * 0.5 + 0.5, 0.0, 1.0);
	float foam = smoothstep(1.6, 0.0, depth) * smoothstep(0.35, 0.85, crest)
		* (1.0 - smoothstep(1500.0, 12000.0, dist));
	base = mix(base, u_foam, foam * 0.75);

	ALBEDO = base;
	NORMAL = normalize((VIEW_MATRIX * vec4(n, 0.0)).xyz);
	ROUGHNESS = mix(0.04, 0.13, smoothstep(1500.0, 60000.0, dist))
		+ clamp(length(slope) * 0.22, 0.0, 0.12) + foam * 0.45;
	SPECULAR = 0.5;
	METALLIC = 0.0;

	float shore;
	if (v_ocean > 0.5) {
		// This is the important path: the zero contour comes from a fixed-resolution
		// planet-space field, not from this mesh's vertices. fwidth gives a one-pixel
		// antialias band without broadening the actual geographic coastline.
		float coast_aa = max(fwidth(visual_h) * 1.15, 0.35);
		shore = 1.0 - smoothstep(-coast_aa, coast_aa, visual_h);
	} else {
		// Lake shoreline: retain the signed interpolation of the lake-level mesh.
		float metres_per_px = max(length(fwidth(v_planet)), 1e-3);
		float depth_slope = max(fwidth(v_signed_depth) / metres_per_px, 1e-6);
		float to_shore = v_signed_depth / depth_slope;
		float cell = dist * u_cell_per_metre;
		float aa = max(metres_per_px * 1.25, 0.30);
		float blur = max(cell * u_shore_blur, aa);
		shore = smoothstep(-aa, blur, to_shore) * v_mask;
	}

	ALPHA = shore * mix(0.62, 1.0, smoothstep(0.0, 7.0, depth));
	FOG = aerial_fog(camera_planet, v_planet);
}