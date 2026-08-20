shader_type spatial;
render_mode cull_back, diffuse_burley, specular_schlick_ggx;
/// Terrain surface.
///
/// Beyond shading vertex-coloured ground this does two jobs the streamer needs:
///
///   * it geomorphs a tile between its own shape and its parent's, so an LOD
///     change is a continuous deformation of one mesh rather than a swap of two;
///   * it breaks up the 8 km macro colour lattice with detail generated in
///     *planet* space, so it stays nailed to the ground when the floating
///     origin re-bases underfoot.
///
/// At aircraft/orbit altitude the actual land/water cut is taken from the global
/// macro texture rather than from the triangle crossing sea level. The lock is
/// deliberately engaged well below the 30 km ocean representation handoff, so
/// crossing that handoff cannot change the apparent geography.

/// 0 draws this tile's own surface, 1 draws its parent's. CUSTOM0 carries the
/// offset between the two per vertex, so a tile can be handed over to or from
/// its parent while the two are geometrically identical -- which is what makes
/// an LOD change invisible without drawing both meshes and dithering between
/// them.
instance uniform float morph : hint_range(0.0, 1.0) = 0.0;
instance uniform float chunk_depth = 0.0;          // quadtree depth, for debug views

// Planet-frame origin of the render frame. Render-space positions are small and
// re-based constantly; adding this back recovers a coordinate that does not move
// under the terrain, which is what the detail noise has to be sampled in.
#include "res://shaders/atmosphere.gdshaderinc"
#include "res://shaders/planet_surface_lookup.gdshaderinc"

uniform vec3 u_origin = vec3(0.0);
uniform sampler2DArray u_orbit_elevation;
uniform float u_orbit_face_res = 192.0;
uniform float u_orbit_coast_start_altitude = 12000.0;

// Debug overlays, driven by TerrainDebug. Both are drawn from the mesh's own UV
// attribute and its instance depth -- nothing the quadtree glyphs also use --
// so agreeing with those glyphs is evidence rather than a tautology.
uniform float u_debug_uv : hint_range(0.0, 1.0) = 0.0;
uniform float u_debug_lod : hint_range(0.0, 1.0) = 0.0;
/// Drops everything the surface is painted with -- vertex colour, mottling,
/// slope rock -- for a plain lambert grey, so shape can be looked at without
/// colour and colour without shape.
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

/// Arrow along +V with a tick along +U. Proportions match the glyph mesh in
/// terrain_debug.gd, which is built from the quadtree instead of from the UVs.
float uv_glyph(vec2 uv) {
	vec2 p = uv - vec2(0.5);
	if (abs(p.x) < 0.045 && p.y > -0.28 && p.y < 0.16) {
		return 1.0;
	}
	float head = (p.y - 0.16) / 0.24;
	if (head >= 0.0 && head <= 1.0 && abs(p.x) < 0.15 * (1.0 - head)) {
		return 1.0;
	}
	if (abs(p.y) < 0.03 && p.x > 0.16 && p.x < 0.34) {
		return 1.0;
	}
	float tip = (p.x - 0.34) / 0.10;
	if (tip >= 0.0 && tip <= 1.0 && abs(p.y) < 0.06 * (1.0 - tip)) {
		return 1.0;
	}
	return 0.0;
}

vec3 depth_color(float d) {
	return 0.5 + 0.5 * cos(TAU * (d * 0.13 + vec3(0.0, 0.33, 0.67)));
}

void fragment() {
	vec3 up = normalize(v_planet);
	vec3 camera_planet = CAMERA_POSITION_WORLD + u_origin;
	float camera_alt = length(camera_planet) - u_planet_radius;

	// Lock the visual coastline before the ocean representation changes. From
	// this altitude upward, terrain LOD and the local/orbit water handoff are not
	// allowed to change which pixels are land or sea.
	if (camera_alt >= u_orbit_coast_start_altitude) {
		float visual_h = surface_visual_height(
			u_orbit_elevation, up, u_orbit_face_res, u_planet_radius);
		if (visual_h < 0.0) {
			discard;
		}
	}

	vec3 wn = normalize((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).xyz);
	float slope = clamp(1.0 - dot(wn, up), 0.0, 1.0);
	float dist = length(VERTEX);
	// Detail is looked up on the sphere, not at the actual altitude, so a skirt
	// hanging below the surface carries the same detail as the edge it hangs
	// from instead of drawing itself along the seam.
	vec3 pd = up * u_planet_radius;

	// Each octave is only added while it is still resolvable; past that it is
	// sub-pixel noise that costs bandwidth and aliases.
	float k_mid = 1.0 - smoothstep(3000.0, 26000.0, dist);
	float k_fine = 1.0 - smoothstep(150.0, 1100.0, dist);
	float k_near = 1.0 - smoothstep(18.0, 120.0, dist);
	float mottle = (vnoise(pd * 0.0016) - 0.5) * k_mid;
	if (k_fine > 0.002) {
		mottle += (vnoise(pd * 0.021) - 0.5) * 0.7 * k_fine;
	}
	if (k_near > 0.002) {
		mottle += (vnoise(pd * 0.35) - 0.5) * 0.9 * k_near;
	}

	vec3 albedo = COLOR.rgb * (1.0 + mottle * u_mottle * 2.0);

	// Anything steep is bedrock, whatever the biome says. This is what keeps a
	// cliff from being painted grassland by an 8 km macro cell, and it hides the
	// macro lattice exactly where the lattice is most visible.
	float rock = smoothstep(0.10, 0.42, slope);
	vec3 rock_col = mix(u_rock_dark, u_rock_light, vnoise(pd * 0.008));
	albedo = mix(albedo, rock_col, rock * u_rock_tint);

	albedo = mix(albedo, vec3(0.34, 0.35, 0.37), u_debug_plain);
	if (u_debug_lod > 0.0) {
		albedo = mix(albedo, depth_color(chunk_depth), u_debug_lod * 0.5);
	}
	if (u_debug_uv > 0.0) {
		float edge = min(min(UV.x, 1.0 - UV.x), min(UV.y, 1.0 - UV.y));
		float border = 1.0 - smoothstep(0.004, 0.010, edge);
		albedo = mix(albedo, vec3(0.02, 0.02, 0.03), border * u_debug_uv);
		albedo = mix(albedo, depth_color(chunk_depth) * 0.30, uv_glyph(UV) * u_debug_uv);
	}

	ALBEDO = albedo;
	ROUGHNESS = mix(0.94, 0.82, rock);
	SPECULAR = 0.12;
	METALLIC = 0.0;
	// The air in front of the surface, applied after lighting. Without it the
	// sky is only ever a backdrop behind the planet.
	FOG = aerial_fog(camera_planet, v_planet);
}
