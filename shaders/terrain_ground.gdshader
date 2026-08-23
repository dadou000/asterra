shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx, ambient_light_disabled;
/// Terrain surface.

instance uniform float morph : hint_range(0.0, 1.0) = 0.0;
instance uniform float chunk_depth = 0.0;

#include "res://shaders/atmosphere.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"
#include "res://shaders/coastline_clipmap.gdshaderinc"
#include "res://shaders/material_clipmap.gdshaderinc"
#include "res://shaders/terrain_relief.gdshaderinc"
#include "res://shaders/ground_detail.gdshaderinc"
#include "res://shaders/biome_surface.gdshaderinc"

uniform vec3 u_origin = vec3(0.0);
uniform sampler2DArray u_orbit_elevation;
uniform float u_orbit_face_res = 192.0;
uniform float u_orbit_coast_start_altitude = 1000000.0;
/// Range past which the ground defers to the whole-planet coastline instead of
/// its own triangles. See the note at the discard in fragment().
uniform float u_coast_ground_cut_m = 110000.0;
uniform float u_debug_uv : hint_range(0.0, 1.0) = 0.0;
uniform float u_debug_lod : hint_range(0.0, 1.0) = 0.0;
uniform float u_debug_plain : hint_range(0.0, 1.0) = 0.0;
/// Diagnostic channel. 1 = slope/rock/forest-cover, 2 = the baked vertex colour
/// alone, 3 = albedo just before the close-range material. Off in normal use.
uniform int u_debug_stage = 0;
/// Forest canopy strength. 0 removes the stand mask entirely.
uniform float u_forest : hint_range(0.0, 1.0) = 1.0;
uniform float u_mottle : hint_range(0.0, 0.4) = 0.14;
/// Continental-scale albedo variation, and how much of it is hue rather than
/// brightness. These are what stop a whole biome reading as one flat wash.
uniform float u_macro_mottle : hint_range(0.0, 0.6) = 0.26;
uniform float u_macro_hue : hint_range(0.0, 0.5) = 0.14;
uniform float u_rock_tint : hint_range(0.0, 1.0) = 0.55;
uniform vec3 u_rock_dark : source_color = vec3(0.115, 0.106, 0.098);
uniform vec3 u_rock_light : source_color = vec3(0.285, 0.268, 0.240);
/// The floating origin reduced modulo GD_ORIGIN_WRAP in double precision on the
/// CPU. Added to the render-space position this gives a planet-anchored detail
/// coordinate that keeps sub-millimetre precision and survives a rebase, which
/// `v_planet` cannot: float32 at 1e6 m quantises to about 6 cm.
uniform vec3 u_detail_origin = vec3(0.0);
/// Observer height above the ground directly below, in metres. Decides whether
/// the camera is looking at the top of the canopy or standing underneath it.
uniform float u_camera_agl = 1000.0;
/// Angular size of one pixel, radians: the camera's vertical field of view
/// divided by the viewport's height in pixels. Used to size the procedural
/// detail footprint from distance rather than from a screen derivative.
///
/// This has to come from the actual viewport. It was a constant baked for a
/// 900-line window, so at 4K every octave selection was three times too coarse
/// and the ground kept the detail budget of a screen a ninth of the area --
/// which is not "correct but small", it is a soft blur where the extra pixels
/// should have been showing leaf litter. The default matches 1600x900 so a
/// material that never gets the uniform still behaves as before.
uniform float u_pixel_angle = 0.00132;
/// Local geometry clipmap handoff. The clipmap owns the fine surface under this
/// tangent-frame square; the global quadtree is discarded there so its coarser
/// approximation can never poke through a negative fine residual. A narrow
/// overlap remains outside the cut and is screen-door faded by the clipmap.
uniform float u_ground_clipmap_cutout = 0.0;
uniform vec3 u_ground_clipmap_frame_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_ground_clipmap_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_ground_clipmap_up = vec3(0.0, 1.0, 0.0);
uniform vec2 u_ground_clipmap_center_plane = vec2(0.0);
uniform float u_ground_clipmap_cut_half_extent = 0.0;
/// Normal-incidence reflectance of the dielectrics ground is made of -- rock,
/// soil, leaf wax. Written out rather than taken from SPECULAR so the lobe in
/// light() cannot silently change scale if the material's SPECULAR moves.
const float GROUND_F0 = 0.02;

varying vec3 v_planet;
varying vec3 v_render;
varying vec4 v_surface;
/// Fraction of direct sun reaching this fragment through the canopy above it.
/// Written in fragment() so light() can apply it to the sun term, which is what
/// makes a sunfleck a pool of light rather than a bright patch of paint.
varying float v_canopy_light;
/// Aerial transmittance for this fragment, written in fragment() so that light()
/// can attenuate the specular lobe it adds. Everything else the surface emits is
/// already attenuated before it leaves fragment().
varying vec3 v_atmo_t;

void vertex() {
	VERTEX += CUSTOM0.xyz * morph;
	v_render = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
	v_planet = v_render + u_origin;
	v_surface = CUSTOM1;
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
	float dist_to_camera = length(VERTEX);

	// Remove the coarse global mesh underneath the local geometry clipmap. The
	// projection is the same tangent-frame mapping used by the clipmap sampler,
	// so the cut cannot drift into a visible square when the floating origin moves.
	if (u_ground_clipmap_cutout > 0.5) {
		vec3 frame_dir = normalize(u_ground_clipmap_frame_dir);
		float denom = dot(up, frame_dir);
		if (denom > 0.08) {
			vec2 plane = vec2(dot(up, normalize(u_ground_clipmap_right)),
				dot(up, normalize(u_ground_clipmap_up))) / denom * u_planet_radius;
			vec2 local_plane = plane - u_ground_clipmap_center_plane;
			if (max(abs(local_plane.x), abs(local_plane.y)) < u_ground_clipmap_cut_half_extent) {
				discard;
			}
		}
	}

	// Cut the ground back to the authoritative coastline wherever the mesh is too
	// coarse to find it itself.
	//
	// The quadtree keeps roughly D/64 metres of vertex spacing at distance D, so
	// past about 130 km a single triangle spans more ground than one texel of the
	// coastline field. On a steep continental margin that triangle runs from
	// summit to sea floor in one straight interpolation and stays above the water
	// for half its length -- so the terrain paints over the ocean, and the visible
	// shoreline becomes the triangle's own sea-level crossing: a chain of dead
	// straight runs tens of kilometres long meeting at sharp corners. It is the
	// single most conspicuous artifact in any view from altitude.
	//
	// This used to fire only above `u_orbit_coast_start_altitude`, which is an
	// orbital figure, so every view between a few kilometres up and orbit had a
	// polygon for a coastline. The trigger is distance to the fragment, because
	// that is what actually decides the mesh's resolution there -- and unlike
	// chunk depth it is smooth, so it cannot stamp quadtree rectangles into the
	// shore.
	float coast_cut = min(u_orbit_coast_start_altitude, camera_alt >= u_orbit_coast_start_altitude
		? 0.0 : u_coast_ground_cut_m);
	if (dist_to_camera > coast_cut) {
		float visual_h = coast_surface_height(u_orbit_elevation, up, u_orbit_face_res, u_planet_radius);
		if (visual_h < 0.0) discard;
	}

	vec3 wn = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);

	// Put back the band of relief this chunk's tessellation dropped. This has to
	// happen before `slope`, so exposed rock follows real mountainsides at
	// distance rather than tracking the sphere.
	float relief_texel = relief_texel_metres(u_orbit_face_res, u_planet_radius);
	if (u_relief_ready > 0.5 && u_relief_normal > 0.0) {
		// Nearby geometry needs only the band its own tessellation omitted. At
		// kilometre altitude, however, neighbouring chunks deliberately have
		// different depths; using depth as a normal-filter scale exposes each node
		// as a long rectangular wedge. Converge to one planet-anchored relief normal
		// before mixed LODs dominate the view. The far branch is also cheaper (one
		// gradient instead of fine-minus-coarse reconstruction).
		float continuous_w = smoothstep(1400.0, 2600.0, dist_to_camera);
		vec3 rn;
		if (continuous_w > 0.995) {
			rn = relief_normal_at(u_orbit_elevation, up, u_orbit_face_res,
				u_planet_radius, relief_texel);
		} else {
			rn = relief_detail_normal(u_orbit_elevation, up, u_orbit_face_res,
				u_planet_radius, relief_mesh_spacing(chunk_depth), wn);
			if (continuous_w > 0.005) {
				vec3 continuous_n = relief_normal_at(u_orbit_elevation, up,
					u_orbit_face_res, u_planet_radius, relief_texel);
				rn = normalize(mix(rn, continuous_n, continuous_w));
			}
		}
		wn = normalize(mix(wn, rn, u_relief_normal));
	}

	float slope = clamp(1.0 - dot(wn, up), 0.0, 1.0);
	float dist = dist_to_camera;
	vec3 pd = up * u_planet_radius;

	// How high this fragment stands above the land around it. Everything that
	// varies with altitude *within* a climate cell reads this: the treeline, the
	// exposure of a ridge, the shelter of a hollow. The mesh supplies the
	// fragment's own height exactly; the elevation texture, averaged over a disc
	// several kilometres wide, supplies the base level to measure it against.
	float alt_anomaly = 0.0;
	if (u_relief_ready > 0.5) {
		float h_here = length(v_planet) - u_planet_radius;
		float h_base = relief_mean_height(u_orbit_elevation, up, u_orbit_face_res,
			u_planet_radius, relief_texel * 2.2);
		alt_anomaly = h_here - h_base;
	}

	float k_mid = 1.0 - smoothstep(3000.0, 26000.0, dist);
	float k_fine = 1.0 - smoothstep(150.0, 1100.0, dist);
	float k_near = 1.0 - smoothstep(18.0, 120.0, dist);
	float coarse_noise = 0.0;
	float detail_noise = 0.0;
	float fine_noise = 0.0;
	float mottle = 0.0;
	if (k_mid > 0.002) {
		coarse_noise = vnoise(pd * 0.0016) - 0.5;
		mottle = coarse_noise * k_mid;
	}
	if (k_fine > 0.002) {
		detail_noise = vnoise(pd * 0.021) - 0.5;
		mottle += detail_noise * 0.7 * k_fine;
	}
	if (k_near > 0.002) {
		fine_noise = vnoise(pd * 0.35) - 0.5;
		mottle += fine_noise * 0.9 * k_near;
	}

	// Continental-scale variegation. Every octave above lives at 600 m or
	// shorter, which is invisible from orbit -- so the whole visible planet was
	// being painted by the 8 km biome grid alone and came out as a handful of
	// smooth blobs. These three bands (roughly 90 km, 22 km and 5 km) are the
	// scales an orbital image is actually made of: provinces, massifs, basins.
	// They never fade out, because distance is exactly when they matter.
	float region = vnoise(pd * 0.000011) - 0.5;
	float province = vnoise(pd * 0.000046 + vec3(19.7, -4.1, 8.3)) - 0.5;
	float district = vnoise(pd * 0.00021 + vec3(-7.3, 11.9, 3.5)) - 0.5;
	// Ridged variant: a folded belt reads as banded, not blotchy.
	float belt = 1.0 - abs(vnoise(pd * 0.000072 + vec3(3.1, 5.5, -9.4)) * 2.0 - 1.0);
	float macro_mottle = region * 1.00 + province * 0.62 + district * 0.38
		+ (belt - 0.5) * 0.44;

	// Precision-safe planet-anchored coordinate for everything finer than a metre.
	vec3 dpos = v_render + u_detail_origin;
	// Pixel footprint from distance, NOT from fwidth(). A screen derivative of an
	// interpolated position is piecewise constant per triangle and jumps at every
	// triangle edge; since the footprint selects which octaves an fBm sums, those
	// jumps change the field's value and stamp the tessellation onto anything
	// thresholded downstream as faceted blobs. Distance is smooth everywhere.
	// The grazing term is capped because a surface seen edge-on has an unbounded
	// footprint and would otherwise erase all detail along the horizon.
	float grazing = clamp(dot(normalize(-VERTEX), NORMAL), 0.12, 1.0);
	float pixel_m = max(dist * u_pixel_angle / grazing, 1e-4);
	// Material classification is virtual-textured independently from geometry.
	// The vertex payload remains the fallback beyond the outer clipmap ring.
	vec4 surf = clamp(material_clipmap_surface(v_planet, u_planet_radius,
		clamp(v_surface, 0.0, 1.0)), 0.0, 1.0);

	// One common tint composition for the baked biome and every photographic
	// constituent layered over it. This is deliberately computed once so a lower
	// material cannot accidentally erase the landscape-scale colour structure.
	float local_colour = mottle * u_mottle;
	vec3 material_tint = vec3(1.0 + local_colour * 1.60,
		1.0 + local_colour * 0.65, 1.0 - local_colour * 1.15);
	// A visible value band is as important as the warm/cool shift. Omitting this
	// left only a few-percent hue change, which atmospheric scattering erased at
	// the exact altitude where the 600 m layer should become most useful.
	material_tint *= 1.0 + local_colour * 2.10;
	material_tint *= 1.0 + macro_mottle * u_macro_mottle;
	material_tint *= vec3(1.0 + macro_mottle * u_macro_hue * 0.55,
		1.0 + macro_mottle * u_macro_hue * 0.10,
		1.0 - macro_mottle * u_macro_hue * 0.45);
	material_tint = max(material_tint, vec3(0.52));
	vec3 albedo = COLOR.rgb * material_tint;
	// Bare rock needs a slope soil cannot hold *and* nothing growing on it. A
	// temperate hillside at thirty degrees is forest, not scree; keyed to
	// steepness alone this painted every sunlit face of a rainforest in bright
	// stone, which is most of the pale patches a mountain landscape had on it.
	//
	// The threshold is also deliberately wide. `slope` comes from the interpolated
	// mesh normal, which is only C0 across a triangle, so a narrow threshold on it
	// resolves the triangulation into hard-edged faceted blobs -- the same trap
	// the forest mask documents and avoids.
	float rock = smoothstep(0.16, 0.62, slope)
		* (1.0 - clamp(surf.r * 1.5, 0.0, 1.0));
	vec3 rock_col = mix(u_rock_dark, u_rock_light,
		clamp(0.5 + mix(coarse_noise, detail_noise, k_fine), 0.0, 1.0));
	albedo = mix(albedo, rock_col, rock * u_rock_tint);
	albedo = biome_albedo(albedo, surf, slope,
		coarse_noise, fine_noise, max(k_fine, k_near));

	vec3 pre_close = albedo;
	float ground_bump = 0.0;
	vec3 ground_normal_offset = vec3(0.0);
	// Seeded with the biome's own roughness rather than a sentinel, because the
	// close-range material now blends into it instead of replacing it.
	float ground_rough = biome_roughness(clamp(v_surface, 0.0, 1.0), slope);

	// Where the trees are. This is a landscape-scale field, so unlike the ground
	// material it stays on out to the horizon -- a forest is one of the few
	// things still legible from orbit.
	// Above the canopy you look at crowns; underneath it you look at the floor.
	// Both are the same forest, but they are opposite sides of it, and the crown
	// model is strictly a top-down one.
	float canopy_view = smoothstep(3.0, 24.0, u_camera_agl);
	float cover = 0.0;
	float forest_fraction = 0.0;
	float forest_patch = 0.0;
	float under_canopy = 0.0;
	v_canopy_light = 1.0;
	if (u_forest > 0.0) {
		// No distance cutoff. There used to be one at 220 km, which drew a hard
		// ring across every view from altitude -- inside it the ground carried the
		// canopy model, outside it did not, and the two did not agree on
		// brightness. The mask's own octaves already fade with the pixel
		// footprint, so out here it costs a warp and two band-limited samples and
		// resolves into a smooth cover fraction on its own.
		vec2 fc = forest_cover(pd, dpos, pixel_m, surf.r, surf.g, slope, surf.a,
			alt_anomaly);
		cover = fc.x * u_forest;
		forest_fraction = fc.y * u_forest;
		// Restore realised forest stands, but retain a little filtered coverage so
		// clearing/stand transitions cannot collapse into the old binary leopard
		// pattern. `cover` is already footprint-antialiased in forest_cover().
		forest_patch = clamp(forest_fraction + (cover - forest_fraction) * 0.82,
			0.0, 1.0);
		// Use the same scanned forest material at every camera height. Mipmaps
		// resolve its detail naturally; only the illumination changes between a
		// forest-floor view and a shaded top-of-canopy view.
		under_canopy = forest_patch * (1.0 - canopy_view);
		// Keep the scanned canopy material active through orbital views. The old
		// 25--90 km fade was almost complete around 86 km and visibly changed the
		// forest's identity. Texture mipmaps, rather than camera distance, now
		// decide which photographed frequencies remain resolvable.
		float forest_texture_w = forest_patch;
		if (forest_texture_w > 0.004) {
			float fleck;
			albedo = forest_surface_textured(albedo, dpos, up, surf.g, canopy_view,
				material_tint, forest_texture_w, ground_rough, ground_normal_offset, fleck);
			v_canopy_light = mix(1.0, fleck, under_canopy);
		}
	}

	// Close-range material. It fades in over a few hundred metres rather than a
	// few tens, because the octave selection inside is footprint-driven and
	// therefore already correct at any range it is switched on for. Under a
	// closed canopy the floor is barely visible, so it yields to the crowns.
	// The floor is only hidden by the canopy when you are looking down onto it.
	// Suppressing the close-range material by cover regardless meant that
	// standing in a wood removed the ground material at exactly the range it was
	// written for.
	// ...and where the forest floor has already answered the question, this must
	// not answer it again. `ground_close` is the open-ground material -- grass
	// tufts, soil, gravel -- and it runs after the floor, so at full strength it
	// simply painted over the litter with a lawn.
	// Do not replace photographed ground with the procedural base at orbital
	// distance. Automatic anisotropic mip selection removes sub-pixel detail but
	// preserves the texture's higher-frequency character without a range seam.
	float close_w = (1.0 - forest_patch * 0.72 * canopy_view)
		* (1.0 - under_canopy * 0.88);
	if (close_w > 0.004) {
		albedo = ground_close_textured(albedo, surf, slope, dpos, up,
			pixel_m, material_tint, close_w, ground_bump, ground_rough,
			ground_normal_offset);
	}

	if (u_debug_stage == 1) {
		albedo = vec3(slope, rock, cover);
	} else if (u_debug_stage == 2) {
		albedo = COLOR.rgb * 6.0;
	} else if (u_debug_stage == 3) {
		albedo = pre_close * 6.0;
	} else if (u_debug_stage == 4) {
		// Reference swatches across the screen. If a known pure green does not
		// arrive green, the fault is in the render path and not in any albedo.
		float band = floor(SCREEN_UV.x * 5.0);
		if (band < 1.0) albedo = vec3(0.30, 0.05, 0.05);
		else if (band < 2.0) albedo = vec3(0.05, 0.30, 0.05);
		else if (band < 3.0) albedo = vec3(0.05, 0.05, 0.30);
		else if (band < 4.0) albedo = vec3(0.035, 0.095, 0.032);
		else albedo = vec3(0.18, 0.18, 0.18);
	}
	albedo = mix(albedo, vec3(0.34, 0.35, 0.37), u_debug_plain);
	if (u_debug_lod > 0.0) albedo = mix(albedo, depth_color(chunk_depth), u_debug_lod * 0.5);
	if (u_debug_uv > 0.0) {
		float edge = min(min(UV.x, 1.0 - UV.x), min(UV.y, 1.0 - UV.y));
		float border = 1.0 - smoothstep(0.004, 0.010, edge);
		albedo = mix(albedo, vec3(0.02, 0.02, 0.03), border * u_debug_uv);
		albedo = mix(albedo, depth_color(chunk_depth) * 0.30, uv_glyph(UV) * u_debug_uv);
	}

	vec3 sun_dir = normalize(u_sun_dir);
	vec3 skylight = planet_sky_irradiance(v_planet, sun_dir, u_planet_radius);
	// Micro relief from the same field that coloured the ground, so the lit sides
	// of tufts and grains agree with where they were drawn. Without this the
	// close-range material is a painted texture on a mathematically flat plane,
	// which is what makes procedural ground read as linoleum.
	// The forest floor needs its relief as much as open ground does, and it is
	// suppressed out of `close_w` on purpose, so it has to be added back here.
	float bump_w = max(max(close_w, under_canopy),
		forest_patch * (1.0 - smoothstep(2000.0, 14000.0, dist)));
	if (bump_w > 0.004) {
		// Litter needs far more relief than a grass sward does. Almost all of the
		// contrast in a close view of a forest floor is the shading of individual
		// pieces of it, not their colour -- a leaf and the leaf beside it are the
		// same brown, and what separates them is that one is tilted.
		float bump_strength = mix(mix(0.040, 0.105, surf.r), 0.15, under_canopy);
		wn = gd_perturb_normal(v_render, wn, ground_bump, bump_strength * bump_w);
	}
	// Photographic OpenGL normal maps supply the close-range blade, leaf and soil
	// aggregate relief. Their offset is already faded by close_w in the material.
	if (dot(ground_normal_offset, ground_normal_offset) > 1e-6) {
		wn = normalize(wn + ground_normal_offset * 0.72);
	}
	NORMAL = normalize((VIEW_MATRIX * vec4(wn, 0.0)).xyz);

	// Large-scale sky occlusion: a valley floor sees a smaller piece of sky than
	// the ridge above it. Without this the terrain shadow has nothing to sit
	// against and shadowed ground reads as flat paint at a lower brightness.
	float sky_occ = 1.0;
	if (u_relief_ready > 0.5 && u_relief_ao > 0.0) {
		sky_occ = relief_sky_occlusion(u_orbit_elevation, up, u_orbit_face_res,
			u_planet_radius, relief_texel * 2.5);
		sky_occ = mix(1.0, sky_occ, u_relief_ao);
	}

	// Aerial perspective, applied by hand rather than through FOG so the
	// transmittance keeps its spectrum. ALBEDO carries it into the diffuse term
	// Godot multiplies out; the specular lobe is attenuated in light().
	vec3 atmo_t;
	vec3 atmo_l;
	aerial_perspective(camera_planet, v_planet, atmo_t, atmo_l);
	v_atmo_t = atmo_t;

	ALBEDO = albedo * atmo_t;
	// The sky term is hemispherical irradiance, not orientation-independent
	// emissive paint. Down-facing gullies see less sky than broad horizontal
	// ground; retaining some fill on vertical rock avoids an artificial void.
	float sky_facing = clamp(dot(wn, up), 0.0, 1.0);
	float sky_exposure = mix(0.32, 1.0, pow(sky_facing, 0.72));
	// Under a canopy the fill light is not the sky, it is the sky *after the
	// leaves*: a few per cent of it, filtered hard toward green. That green glow
	// is the characteristic light of a wood, and without it a forest floor with a
	// physically correct canopy over it simply renders black.
	vec3 fill = mix(skylight, skylight * vec3(0.52, 1.18, 0.56) * 2.1, under_canopy);
	EMISSION = albedo * fill * sky_exposure * sky_occ * atmo_t + atmo_l;
	ROUGHNESS = mix(ground_rough, 0.82, rock * 0.35);
	SPECULAR = 0.12;
	METALLIC = 0.0;
	AO = sky_occ;
	AO_LIGHT_AFFECT = 1.0;
}

void light() {
	float visibility = 1.0;
	if (LIGHT_IS_DIRECTIONAL) {
		vec3 sun_dir = normalize(u_sun_dir);
		visibility = planet_direct_sun_visibility(v_planet, sun_dir, u_planet_radius);
		// Cast shadows from the terrain itself. Godot's shadow map is capped at a
		// few kilometres, so beyond that this is the only thing that puts a
		// mountain's shadow in the valley next to it. The two multiply rather
		// than replace each other: the map owns the metre scale it can see, this
		// owns the kilometre scale it cannot.
		if (u_relief_ready > 0.5 && u_relief_shadow > 0.0 && visibility > 0.001) {
			float cast_vis = relief_sun_shadow(u_orbit_elevation, v_planet, sun_dir,
				u_orbit_face_res, u_planet_radius);
			visibility *= mix(1.0, cast_vis, u_relief_shadow);
		}
	}

	float ndotl = max(dot(NORMAL, LIGHT), 0.0);
	visibility *= v_canopy_light;
	DIFFUSE_LIGHT += ndotl * ATTENUATION * LIGHT_COLOR * visibility / PI;

	if (ndotl > 0.0) {
		// Normalised Blinn-Phong. The previous form scaled a raw `pow` by a flat
		// 0.22, so as roughness fell the lobe grew both tighter *and* brighter --
		// the opposite of how a microfacet distribution behaves, where narrowing
		// the lobe is what pays for its height. A merely damp hillside therefore
		// grew a highlight an order of magnitude above its own diffuse.
		//
		// Every energy term below is load-bearing. (n+8)/8pi normalises the lobe,
		// `ndotl` is the projected area the light actually falls on, and GROUND_F0
		// is a dielectric's normal-incidence reflectance. Drop any one of them and
		// the result is not a wrong highlight but a broad white veil over the
		// entire sunlit side of the landscape, because at the roughness real
		// ground has the lobe spans most of the hemisphere.
		vec3 h = normalize(LIGHT + VIEW);
		float ndoth = max(dot(NORMAL, h), 0.0);
		float r = clamp(ROUGHNESS, 0.05, 1.0);
		float r4 = r * r * r * r;
		float spec_power = clamp(2.0 / r4 - 2.0, 1.0, 2048.0);
		float lobe = pow(ndoth, spec_power) * (spec_power + 8.0) / (8.0 * PI);
		SPECULAR_LIGHT += LIGHT_COLOR * ATTENUATION * visibility * ndotl * lobe
			* GROUND_F0 * v_atmo_t;
	}
}