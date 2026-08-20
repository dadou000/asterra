shader_type spatial;
render_mode blend_mix, cull_disabled, depth_draw_always, diffuse_burley, specular_schlick_ggx;
/// Free water: oceans and lakes.
///
/// The mesh stays geometrically flat -- it is a shell at the water level, shared
/// with the LOD skirt system -- and every wave lives in the normal. That is what
/// lets the same shader be chop at wading depth and a mirror from orbit: the
/// wave octaves are faded out individually as they stop being resolvable, so
/// distance costs detail instead of aliasing it.
///
/// Above the orbit handoff altitude this chunk-bound representation is discarded
/// completely. A global ocean shell takes over there so coastline shape no longer
/// depends on the streamed mesh resolution.

/// 0 draws this tile's own surface, 1 draws its parent's. CUSTOM0 carries the
/// offset between the two per vertex, so a tile can be handed over to or from
/// its parent while the two are geometrically identical -- which is what makes
/// an LOD change invisible without drawing both meshes and dithering between
/// them.
instance uniform float morph : hint_range(0.0, 1.0) = 0.0;

#include "res://shaders/atmosphere.gdshaderinc"

uniform vec3 u_origin = vec3(0.0);
uniform vec3 u_deep : source_color = vec3(0.022, 0.082, 0.165);
uniform vec3 u_shallow : source_color = vec3(0.090, 0.395, 0.445);
uniform vec3 u_foam : source_color = vec3(0.82, 0.90, 0.94);
uniform float u_wave_scale : hint_range(0.0, 3.0) = 1.0;
uniform float u_depth_scale = 4000.0;    // metres encoded in COLOR.r, fourth-root curve
uniform float u_deep_at = 42.0;          // metres at which the deep colour is reached
uniform float u_orbit_handoff_altitude = 30000.0;
/// How much of one mesh cell's worth of shoreline uncertainty the waterline
/// fades over. 1.0 blurs it across exactly the distance the mesh cannot resolve.
uniform float u_shore_blur : hint_range(0.0, 3.0) = 0.9;
/// Metres of vertex spacing per metre of view distance. The quadtree splits on
/// distance, so tile resolution is a function of distance -- and taking the
/// spacing from the distance rather than from the tile keeps two tiles that meet
/// at an LOD boundary agreeing about where the shoreline is. Reading it off the
/// tile instead draws the LOD boundary itself across the water.
uniform float u_cell_per_metre = 0.0223;

varying vec3 v_planet;
varying float v_depth;
varying float v_mask;

void vertex() {
	VERTEX += CUSTOM0.xyz * morph;
	v_planet = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz + u_origin;
	// Depth is stored on a fourth-root curve, so 8-bit vertex colour resolves
	// about a decimetre at the waterline -- where the shoreline fade needs it --
	// while still reaching the abyss, which the fade needs a gradient from.
	float r2 = COLOR.r * COLOR.r;
	v_depth = r2 * r2 * u_depth_scale;
	v_mask = COLOR.g;
}

/// How much of an octave survives at this distance. A wave has to be several
/// pixels across to be a wave; past that it is noise in the normal, and a normal
/// sampled below its own frequency is exactly how a calm sea turns into a moire
/// of glitter. Faded out, its energy is handed to ROUGHNESS instead.
float octave(float wavelength, float dist) {
	return 1.0 - smoothstep(wavelength * 45.0, wavelength * 190.0, dist);
}

/// One plane wave. `w` is the angular frequency from the deep-water dispersion
/// relation, so long swell rolls slowly and ripples skitter.
///
/// The phase is warped by a much longer wave running across it. Pure sines sum
/// into a regular lattice -- a corduroy that reads as a rendering artefact, not
/// as sea -- and bending the crests along their length is the cheapest way to
/// break that up without paying for noise.
void wave(vec3 ps, vec3 up, vec3 dir, vec3 cross_dir, float k, float amp, float w, float t,
		inout float h, inout vec3 grad) {
	if (amp <= 0.0) {
		return;
	}
	float warp = sin(dot(ps, cross_dir) * k * 0.21 + t * w * 0.23);
	float ph = dot(ps, dir) * k + t * w + warp * 1.7;
	h += amp * sin(ph);
	// Only the part of the wave vector lying in the surface tilts the surface;
	// the radial part just moves the whole sheet up and down.
	grad += (dir - up * dot(dir, up)) * (amp * k * cos(ph));
}

void fragment() {
	vec3 camera_planet = CAMERA_POSITION_WORLD + u_origin;
	float camera_alt = length(camera_planet) - u_planet_radius;
	if (camera_alt >= u_orbit_handoff_altitude) {
		discard;
	}

	vec3 up = normalize(v_planet);
	float dist = length(VERTEX);
	float t = TIME;
	// Waves are phased on the position *projected back onto the sphere*, so the
	// phase depends on where you are on the planet and not on how high you are.
	// The skirt that bridges two water tiles hangs below the surface: phased on
	// its own position it carries a different wave than the tile it belongs to,
	// and draws itself as a line along every chunk edge in the ocean.
	vec3 ps = v_planet * (u_planet_radius / max(1.0, length(v_planet)));

	// Shallow water carries less energy: the swell shortens and dies as the
	// bottom comes up, which is what makes a coastline read as a coastline.
	float shoaling = clamp(v_depth / 6.0, 0.06, 1.0);
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

	float depth_t = clamp(v_depth / u_deep_at, 0.0, 1.0);
	vec3 base = mix(u_shallow, u_deep, depth_t * (2.0 - depth_t));

	// Foam where the water is thin enough for the wave crests to reach the bed.
	float crest = clamp(h / max(0.15, 0.9 * a) * 0.5 + 0.5, 0.0, 1.0);
	// Foam is surf: it is real at wading distance and sub-pixel from a plane.
	float foam = smoothstep(1.6, 0.0, v_depth) * smoothstep(0.35, 0.85, crest)
		* (1.0 - smoothstep(1500.0, 12000.0, dist));
	base = mix(base, u_foam, foam * 0.75);

	ALBEDO = base;
	NORMAL = normalize((VIEW_MATRIX * vec4(n, 0.0)).xyz);
	// Roughness carries the chop that is no longer in the normal -- both the
	// octaves distance has taken away and the sub-pixel part of the ones still
	// there. Without that second term the sun glint aliases into a checkerboard
	// the moment the wave slope gets steeper than the pixel it is drawn in.
	ROUGHNESS = mix(0.04, 0.13, smoothstep(1500.0, 60000.0, dist))
		+ clamp(length(slope) * 0.22, 0.0, 0.12) + foam * 0.45;
	SPECULAR = 0.5;
	METALLIC = 0.0;

	// Dissolve at the waterline instead of ending on a polygon edge, and let the
	// shallows show the bed through them -- `v_mask` is how much the mesher
	// trusts this as free water at all. Past wading depth it goes fully opaque,
	// which is both true and what keeps the LOD skirts from showing: a skirt
	// overlaps its neighbour's surface by a pixel or so at grazing angles, and
	// two translucent layers over one sea bed draw a darker line along every
	// chunk edge in the ocean.
	// The waterline is where the ground crosses the water level, and the mesh
	// only knows where that is to within one cell. Ending the surface there --
	// on the geometric edge, however deep the water happens to be one cell
	// further out -- draws the coastline as the straight edges of the triangles
	// it was clipped against, which from orbit are 25 km wide and turn a coast
	// into a polygon.
	//
	// So the fade is a *horizontal* distance, not a depth: depth divided by the
	// depth gradient is roughly how far away the shoreline is, and the surface
	// dissolves over one cell's worth of it. On a tile you are standing on that
	// is sub-metre and invisible; on a tile sampled every 25 km the water gives
	// way over a whole triangle and the smooth sea-bed colour underneath draws
	// the coast instead. Fading in depth cannot do this: next to a continent the
	// sea bed drops kilometres within one cell, so any depth band is a hairline.
	float metres_per_px = max(length(fwidth(v_planet)), 1e-3);
	float depth_slope = max(fwidth(v_depth) / metres_per_px, 1e-6);
	float to_shore = v_depth / depth_slope;
	float cell = dist * u_cell_per_metre;
	float shore = smoothstep(0.0, max(cell * u_shore_blur, 0.35), to_shore) * v_mask;
	ALPHA = shore * mix(0.62, 1.0, smoothstep(0.0, 7.0, v_depth));

	FOG = aerial_fog(camera_planet, v_planet);
}
