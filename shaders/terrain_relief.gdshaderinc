// Large-scale relief shading from the whole-planet elevation field.
//
// The quadtree mesh is the only thing that carries terrain shape, and at any
// distance the mesh has already been decimated to a few hundred metres of
// silhouette. A mountain range 3 km high across 40 km of ground therefore
// arrives at the fragment stage as a normal that is, to within a fraction of a
// degree, the sphere normal -- which is why a planet lit this way reads as a
// painted ball rather than a landscape.
//
// Everything here rebuilds the missing shading from `u_orbit_elevation`, the
// same cube-sphere height texture the coastline already samples. It is ~2 km
// per texel at the default resolution, which is finer than the mesh at every
// LOD above the local one, so it restores exactly the band of relief the mesh
// threw away:
//
//   * a true surface normal by central differences on the sphere,
//   * cast sun shadows by horizon marching along the solar azimuth,
//   * sky occlusion from the surrounding horizon ring.
//
// Shadows are the important one. Godot's directional shadow map is capped at a
// few kilometres for cost reasons and contributes nothing past that, so without
// this a mountain never darkens the valley beside it.
//
// Requires planet_surface_lookup.gdshaderinc.

uniform float u_relief_ready = 0.0;
uniform float u_relief_normal : hint_range(0.0, 1.0) = 1.0;
uniform float u_relief_shadow : hint_range(0.0, 1.0) = 1.0;
uniform float u_relief_ao : hint_range(0.0, 1.0) = 1.0;
/// Mesh vertex spacing at quadtree root, metres. The CPU knows the chunk
/// tessellation; the shader uses it to decide when the texture is the better
/// source of normals than the geometry it is drawing.
uniform float u_relief_root_spacing = 49000.0;

// Exponential shadow spacing keeps distant ridge coverage with fewer taps than
// the former 24-step march. Four horizon axes are enough for the deliberately
// broad sky-visibility term and halve its cost at aircraft/orbit coverage.
const int RELIEF_SHADOW_STEPS = 16;
const int RELIEF_AO_DIRECTIONS = 4;
/// Tallest terrain worth casting a shadow from. Sets how far the march has to
/// reach when the sun is near the horizon.
const float RELIEF_MAX_TERRAIN_M = 6000.0;
const float RELIEF_MAX_SHADOW_M = 90000.0;

/// Mesh vertex spacing in metres for a quadtree node at `depth`.
float relief_mesh_spacing(float depth) {
	return u_relief_root_spacing / exp2(depth);
}

/// Texel footprint of the elevation texture in metres.
float relief_texel_metres(float face_res, float radius) {
	return (PI * 0.5 * radius) / max(face_res, 1.0);
}

/// Single-tap height. The shadow march wants many samples and does not need the
/// seam-exact reconstruction that the coastline depends on.
float relief_height(sampler2DArray tex, vec3 dir, float face_res) {
	return texture(tex, surface_array_coord(dir, face_res)).r;
}

/// Land surface height: below sea level the water plane is what casts and
/// receives, not the sea floor.
float relief_land_height(sampler2DArray tex, vec3 dir, float face_res) {
	return max(relief_height(tex, dir, face_res), 0.0);
}

/// Orthonormal tangent frame. The gradient it recovers is basis-independent, so
/// the branch only has to avoid a degenerate cross product.
void relief_basis(vec3 up, out vec3 t, out vec3 b) {
	vec3 seed = abs(up.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	t = normalize(cross(seed, up));
	b = cross(up, t);
}

/// Mean land height over a disc of radius `step_m`, as a five-tap average.
///
/// This is the local base level a fragment's own altitude should be measured
/// against. The difference between the two is what says "ridge" or "hollow", and
/// -- via the lapse rate -- where the treeline falls, both of which have to vary
/// smoothly across the surface rather than stepping at the boundary of whatever
/// cell the climate model averaged over.
float relief_mean_height(sampler2DArray tex, vec3 dir, float face_res, float radius,
		float step_m) {
	vec3 up = normalize(dir);
	vec3 t;
	vec3 b;
	relief_basis(up, t, b);
	float ds = step_m / radius;
	float h = relief_land_height(tex, up, face_res) * 2.0;
	h += relief_land_height(tex, normalize(up + t * ds), face_res);
	h += relief_land_height(tex, normalize(up - t * ds), face_res);
	h += relief_land_height(tex, normalize(up + b * ds), face_res);
	h += relief_land_height(tex, normalize(up - b * ds), face_res);
	return h / 6.0;
}

/// Tangential height gradient by central differences on the sphere, at a chosen
/// baseline. Returned as a slope vector rather than a normal so that gradients
/// measured at two different baselines can be subtracted -- which is how the
/// band the mesh is missing gets isolated below.
vec3 relief_slope_at(sampler2DArray tex, vec3 dir, float face_res, float radius,
		float step_m) {
	vec3 up = normalize(dir);
	vec3 t;
	vec3 b;
	relief_basis(up, t, b);
	float ds = step_m / radius;
	float hx1 = relief_land_height(tex, normalize(up + t * ds), face_res);
	float hx0 = relief_land_height(tex, normalize(up - t * ds), face_res);
	float hy1 = relief_land_height(tex, normalize(up + b * ds), face_res);
	float hy0 = relief_land_height(tex, normalize(up - b * ds), face_res);
	float inv = 1.0 / (2.0 * step_m);
	return t * ((hx1 - hx0) * inv) + b * ((hy1 - hy0) * inv);
}

/// Slope vector implied by a normal, and its inverse. `n = normalize(up - s)`.
vec3 relief_slope_of(vec3 n, vec3 up) {
	float c = max(dot(n, up), 1e-3);
	return -(n - up * c) / c;
}

vec3 relief_normal_of(vec3 slope, vec3 up) {
	return normalize(up - slope);
}

vec3 relief_normal_at(sampler2DArray tex, vec3 dir, float face_res, float radius,
		float step_m) {
	return relief_normal_of(relief_slope_at(tex, dir, face_res, radius, step_m),
		normalize(dir));
}

/// Add back exactly the relief the mesh threw away, and nothing else.
///
/// Blending wholesale between the mesh normal and the texture normal by LOD
/// looks right in isolation and is wrong in practice: chunk depth steps by one
/// across a quadtree boundary, so the blend steps with it and stamps visible
/// rectangles across the terrain. Instead, measure the surface twice -- once at
/// texel scale, once at the mesh's own vertex spacing -- and add only the
/// difference. Where the mesh already resolves the texture the two agree and the
/// correction vanishes on its own, so neighbouring LODs disagree only by the
/// sliver of spectrum between them and the seam goes away.
/// How much coarser than its vertex spacing a chunk mesh actually is.
///
/// TerrainDetail band-limits its synthesis to several samples per wavelength
/// rather than to Nyquist, because a linearly interpolated mesh renders anything
/// near Nyquist as a visible zigzag. The mesh therefore carries nothing finer
/// than this multiple of its vertex spacing, and the correction below has to
/// know that or it will decline to restore a band that is genuinely missing.
/// Keep in step with TerrainDetail.SAMPLES_PER_WAVELENGTH.
const float RELIEF_MESH_BAND = 6.0;

vec3 relief_detail_normal(sampler2DArray tex, vec3 dir, float face_res, float radius,
		float mesh_step, vec3 mesh_normal) {
	float texel = relief_texel_metres(face_res, radius);
	float coarse = max(mesh_step * RELIEF_MESH_BAND, texel);
	if (coarse <= texel * 1.02) return mesh_normal;
	vec3 up = normalize(dir);
	vec3 fine_slope = relief_slope_at(tex, dir, face_res, radius, texel);
	vec3 coarse_slope = relief_slope_at(tex, dir, face_res, radius, coarse);
	return relief_normal_of(relief_slope_of(mesh_normal, up)
		+ (fine_slope - coarse_slope), up);
}

/// How far a shadow can reach for this solar elevation. Low sun throws long
/// shadows; marching that distance when the sun is high is wasted samples.
float relief_shadow_reach(float sin_elev) {
	float cos_elev = sqrt(max(1.0 - sin_elev * sin_elev, 0.0));
	float tan_elev = sin_elev / max(cos_elev, 1e-3);
	return clamp(RELIEF_MAX_TERRAIN_M / max(tan_elev, 0.02), 4000.0, RELIEF_MAX_SHADOW_M);
}

/// Cast shadow from the surrounding terrain, by horizon marching along the sun
/// azimuth. Returns 1 in full sun, 0 fully shadowed.
///
/// Every sample is compared in the frame of the shaded point: the occluder's
/// elevation angle above that point's own horizontal, against the sun's. This is
/// what makes the test correct on a sphere -- terrain far enough away curves
/// below the horizon and stops occluding on its own, with no range cutoff.
float relief_sun_shadow(sampler2DArray tex, vec3 dir, vec3 sun_dir, float face_res,
		float radius) {
	vec3 up = normalize(dir);
	float sin_elev = dot(up, sun_dir);
	if (sin_elev <= 0.0) return 1.0;      // night; the planet occultation owns this

	vec3 t = sun_dir - up * sin_elev;
	float tl = length(t);
	if (tl < 1e-4) return 1.0;            // sun at zenith, nothing can occlude
	t /= tl;

	float h0 = relief_land_height(tex, up, face_res);
	vec3 p = up * (radius + h0);
	float reach = relief_shadow_reach(sin_elev);
	float texel = relief_texel_metres(face_res, radius);
	float near = min(texel, reach * 0.5);

	// Exponential spacing between one texel and the full reach. Quadratic or
	// linear spacing leaves gaps of many texels out where the ridges are, and a
	// march that steps straight over an occluder turns its shadow on and off
	// across a pixel -- which is what stamps hard blocks into the terrain. Here
	// every step advances by a fixed *fraction* of the distance already covered,
	// matching the way an occluder's angular size shrinks with range.
	float ratio = pow(max(reach / near, 1.0001), 1.0 / float(RELIEF_SHADOW_STEPS));
	// Per-pixel jitter on the first step, so whatever aliasing survives breaks up
	// into noise instead of arriving as a visible ring.
	float dither = fract(sin(dot(up, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
	float d = near * mix(0.6, 1.4, dither);

	float highest = -1.0;
	for (int i = 0; i < RELIEF_SHADOW_STEPS; i++) {
		float ang = d / radius;
		vec3 q_dir = normalize(up * cos(ang) + t * sin(ang));
		vec3 q = q_dir * (radius + relief_land_height(tex, q_dir, face_res));
		vec3 v = q - p;
		float vl = length(v);
		if (vl > 1.0) {
			highest = max(highest, dot(v, up) / vl);
		}
		d *= ratio;
	}

	// The sun is a disc and the height field is band-limited to a texel, so the
	// shadow edge is inherently soft. Keeping the penumbra wider than the
	// sampling error is what stops the remaining error reading as geometry.
	const float PENUMBRA = 0.030;
	return 1.0 - smoothstep(sin_elev - PENUMBRA, sin_elev + PENUMBRA, highest);
}

/// Sky occlusion from the horizon ring: a valley floor sees less sky than a
/// summit. This is the large-scale ambient term the mesh cannot provide, and it
/// is what stops shadowed ground from going flat black once the sun term is
/// doing real work.
float relief_sky_occlusion(sampler2DArray tex, vec3 dir, float face_res, float radius,
		float sample_m) {
	vec3 up = normalize(dir);
	vec3 t;
	vec3 b;
	relief_basis(up, t, b);
	float h0 = relief_land_height(tex, up, face_res);
	vec3 p = up * (radius + h0);

	float occ = 0.0;
	for (int i = 0; i < RELIEF_AO_DIRECTIONS; i++) {
		float a = TAU * float(i) / float(RELIEF_AO_DIRECTIONS);
		vec3 azim = t * cos(a) + b * sin(a);
		float ang = sample_m / radius;
		vec3 q_dir = normalize(up * cos(ang) + azim * sin(ang));
		vec3 q = q_dir * (radius + relief_land_height(tex, q_dir, face_res));
		vec3 v = q - p;
		float vl = length(v);
		if (vl < 1.0) continue;
		occ += max(dot(v, up) / vl, 0.0);
	}
	return clamp(1.0 - occ / float(RELIEF_AO_DIRECTIONS), 0.0, 1.0);
}
