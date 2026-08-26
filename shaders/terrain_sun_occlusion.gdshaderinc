// Long-range terrain self-shadowing for the active spherical geometry clipmap.
//
// Godot's directional cascades deliberately retain a short range so nearby
// structures and fine terrain receive dense shadow texels. This helper supplies
// the missing kilometre-scale part: it marches the resident whole-planet height
// field along Helion's azimuth and asks whether a mountain rises above the solar
// elevation seen by the actual displaced receiver point.
//
// The receiver is NOT reconstructed from the macro map. Local GPU geomorph and
// microgeometry may place the visible surface above or below the coarse global
// height, and using the coarse receiver there causes false self-shadowing.
//
// Designed for vertex-stage use: all texture reads use explicit LOD and the
// resulting visibility is interpolated across the already dense clipmap mesh.
// Requires planet_surface_lookup.gdshaderinc for surface_face_uv().

const int TERRAIN_SUN_OCCLUSION_MAX_STEPS = 12;
const float TERRAIN_SUN_OCCLUSION_MAX_HEIGHT_M = 8000.0;
const float TERRAIN_SUN_OCCLUSION_MIN_REACH_M = 4000.0;
const float TERRAIN_SUN_OCCLUSION_MAX_REACH_M = 100000.0;
const float TERRAIN_SUN_OCCLUSION_MIN_SAMPLE_M = 900.0;
// Wider than Helion's physical ~0.27 degree angular radius because the global
// height field is band-limited. This absorbs sub-texel ridge uncertainty without
// turning deep mountain shadows into grey penumbrae.
const float TERRAIN_SUN_OCCLUSION_FILTER_RAD = 0.0105;

vec3 terrain_sun_macro_coord(sampler2DArray tex, vec3 dir, float face_res) {
	vec3 fuv = surface_face_uv(dir);
	ivec3 size3 = textureSize(tex, 0);
	float texture_res = float(max(size3.x, 1));
	// Runtime global maps normally carry one cross-face texel on each side, but
	// derive the gutter from the actual texture so this also stays correct if the
	// resident map resolution/layout changes later.
	float gutter = max((texture_res - face_res) * 0.5, 0.0);
	vec2 uv = (fuv.xy * face_res + vec2(gutter)) / texture_res;
	return vec3(uv, fuv.z);
}

float terrain_sun_macro_land_height(sampler2DArray tex, vec3 dir, float face_res) {
	float h = textureLod(tex, terrain_sun_macro_coord(tex, dir, face_res), 0.0).r;
	// Ocean surface, not the abyssal floor, is the relevant horizon at sea.
	return max(h, 0.0);
}

float terrain_sun_macro_texel_m(float face_res, float planet_radius) {
	return (PI * 0.5 * planet_radius) / max(face_res, 1.0);
}

float terrain_sun_shadow_reach(float sin_elev) {
	float cos_elev = sqrt(max(1.0 - sin_elev * sin_elev, 0.0));
	float tan_elev = sin_elev / max(cos_elev, 1e-3);
	return clamp(
		TERRAIN_SUN_OCCLUSION_MAX_HEIGHT_M / max(tan_elev, 0.02),
		TERRAIN_SUN_OCCLUSION_MIN_REACH_M,
		TERRAIN_SUN_OCCLUSION_MAX_REACH_M);
}

// 1 = direct Helion visible, 0 = fully hidden by terrain.
float terrain_sun_visibility(sampler2DArray tex, vec3 surface_position,
		vec3 sun_dir, float face_res, float planet_radius, int requested_steps) {
	if (face_res <= 0.5 || planet_radius <= 1.0) return 1.0;

	vec3 up = normalize(surface_position);
	vec3 light_dir = normalize(sun_dir);
	float sin_elev = dot(up, light_dir);
	// Planet/night visibility is handled separately by planet_direct_sun_visibility.
	// Returning one here avoids spending a horizon march on a light that cannot
	// reach the receiver anyway.
	if (sin_elev <= 0.0) return 1.0;

	vec3 tangent = light_dir - up * sin_elev;
	float tangent_len = length(tangent);
	if (tangent_len < 1e-4) return 1.0; // Helion at zenith.
	tangent /= tangent_len;

	int steps = clamp(requested_steps, 4, TERRAIN_SUN_OCCLUSION_MAX_STEPS);
	float reach = terrain_sun_shadow_reach(sin_elev);
	float texel_m = terrain_sun_macro_texel_m(face_res, planet_radius);
	// Start beyond roughly one macro texel. Fine local occlusion inside this radius
	// belongs to Godot's high-resolution cascades; skipping it also prevents the
	// coarse macro surface from shadowing a locally displaced receiver against itself.
	float first_d = max(TERRAIN_SUN_OCCLUSION_MIN_SAMPLE_M, texel_m * 1.25);
	first_d = min(first_d, reach * 0.5);
	float ratio = pow(max(reach / max(first_d, 1.0), 1.0001), 1.0 / float(steps));

	// Planet-anchored dither breaks the remaining logarithmic sample rings into a
	// stable high-frequency error instead of a moving band as the camera translates.
	float dither = fract(sin(dot(up, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
	float distance_m = first_d * mix(0.72, 1.28, dither);
	float highest_sin = -1.0;

	for (int i = 0; i < TERRAIN_SUN_OCCLUSION_MAX_STEPS; i++) {
		if (i >= steps) break;
		float angle = distance_m / planet_radius;
		vec3 sample_dir = normalize(up * cos(angle) + tangent * sin(angle));
		float sample_h = terrain_sun_macro_land_height(tex, sample_dir, face_res);
		vec3 sample_position = sample_dir * (planet_radius + sample_h);
		vec3 to_occluder = sample_position - surface_position;
		float occluder_distance = length(to_occluder);
		if (occluder_distance > 1.0) {
			highest_sin = max(highest_sin, dot(to_occluder, up) / occluder_distance);
		}
		distance_m *= ratio;
	}

	// Compare elevation sine directly. A large mountain several degrees above
	// Helion still resolves to exactly zero; the filter only softens the uncertain
	// edge imposed by the finite global-height texel footprint.
	return 1.0 - smoothstep(
		sin_elev - TERRAIN_SUN_OCCLUSION_FILTER_RAD,
		sin_elev + TERRAIN_SUN_OCCLUSION_FILTER_RAD,
		highest_sin);
}
