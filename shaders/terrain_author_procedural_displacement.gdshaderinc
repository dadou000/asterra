// Authored displacement wrapper for the generated/cached terrain path.
// The caller supplies each exact LOD lattice sample before triangle interpolation,
// so L-specific authored graphs remain watertight through parent/child morphing.

#include "res://shaders/terrain_author_displacement_bytecode.gdshaderinc"

vec2 author_procedural_sample(vec3 direction, int level, vec2 sample) {
	// Keep the normal game/editor path completely outside the bytecode interpreter.
	// The interpreter owns a large local register array; entering its call path for
	// every terrain and normal sample can exhaust/timeout some Vulkan drivers even
	// when the first instruction would immediately return for an empty program.
	if (u_author_disp_ready < 0.5 || u_author_disp_count <= 0 || u_author_disp_output < 0) {
		return sample;
	}
	float biome = asterra_ctx_biome_id(direction);
	sample.y += author_displacement_height(direction, sample.y, biome, level, TIME);
	return sample;
}

vec2 author_procedural_lattice_sample(ivec2 cell, int level, vec2 sample) {
	float spacing = ring_spacing(level);
	vec2 offset_m = vec2(cell) * spacing;
	vec3 direction = direction_for_basis_offset(
		u_anchor_dir, u_anchor_right, u_anchor_up, offset_m);
	return author_procedural_sample(direction, level, sample);
}
