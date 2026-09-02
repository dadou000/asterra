// Low-cost stochastic de-periodisation for scanned terrain materials.
//
// Full stochastic texture bombing would multiply the already expensive
// triplanar albedo/roughness/normal sample count. Instead we continuously warp
// the shared precision-safe detail position with periodic vector value noise.
//
// Phase 33 makes the cells, offsets and seeds graph-configurable. The runtime
// snaps cell sizes to exact divisors of the 4096 m detail wrap and supplies the
// matching integer periods, preserving floating-origin phase continuity.

uniform float u_pbr_antitile_strength : hint_range(0.0, 1.5) = 1.0;
uniform float u_pbr_antitile_coarse_cell_m : hint_range(1.0, 512.0) = 32.0;
uniform float u_pbr_antitile_fine_cell_m : hint_range(1.0, 512.0) = 8.0;
uniform int u_pbr_antitile_coarse_period = 128;
uniform int u_pbr_antitile_fine_period = 512;
uniform float u_pbr_antitile_coarse_offset_m : hint_range(0.0, 8.0) = 0.58;
uniform float u_pbr_antitile_fine_offset_m : hint_range(0.0, 8.0) = 0.14;
uniform int u_pbr_antitile_coarse_seed = 29093;
uniform int u_pbr_antitile_fine_seed = 46141;

const float SPAT_WRAP_M = 4096.0;

int spat_posmod(int value, int period) {
	int m = value % period;
	return m < 0 ? m + period : m;
}

ivec3 spat_wrap_cell(ivec3 cell, int period) {
	int safe_period = max(period, 1);
	return ivec3(
		spat_posmod(cell.x, safe_period),
		spat_posmod(cell.y, safe_period),
		spat_posmod(cell.z, safe_period));
}

vec3 spat_rand3(ivec3 cell, int period, uint seed) {
	uint h = gm_hash(spat_wrap_cell(cell, period), seed);
	vec3 r = vec3(
		float(h & 0x000003ffu),
		float((h >> uint(10)) & 0x000003ffu),
		float((h >> uint(20)) & 0x000003ffu));
	return r / 511.5 - 1.0;
}

vec3 spat_periodic_value(vec3 p, int period, uint seed) {
	ivec3 i = ivec3(floor(p));
	vec3 f = fract(p);
	// Quintic interpolation keeps the first derivative zero on lattice borders,
	// which is important because textureGrad sees the derivative of the warped
	// coordinate downstream.
	f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

	vec3 c000 = spat_rand3(i + ivec3(0, 0, 0), period, seed);
	vec3 c100 = spat_rand3(i + ivec3(1, 0, 0), period, seed);
	vec3 c010 = spat_rand3(i + ivec3(0, 1, 0), period, seed);
	vec3 c110 = spat_rand3(i + ivec3(1, 1, 0), period, seed);
	vec3 c001 = spat_rand3(i + ivec3(0, 0, 1), period, seed);
	vec3 c101 = spat_rand3(i + ivec3(1, 0, 1), period, seed);
	vec3 c011 = spat_rand3(i + ivec3(0, 1, 1), period, seed);
	vec3 c111 = spat_rand3(i + ivec3(1, 1, 1), period, seed);

	vec3 z0 = mix(mix(c000, c100, f.x), mix(c010, c110, f.x), f.y);
	vec3 z1 = mix(mix(c001, c101, f.x), mix(c011, c111, f.x), f.y);
	return mix(z0, z1, f.z);
}

vec3 spbr_antitile_position(vec3 detail_position) {
	if (u_pbr_antitile_strength <= 0.001) return detail_position;

	float coarse_cell = max(u_pbr_antitile_coarse_cell_m, 0.001);
	float fine_cell = max(u_pbr_antitile_fine_cell_m, 0.001);
	vec3 coarse = spat_periodic_value(
		detail_position / coarse_cell,
		max(u_pbr_antitile_coarse_period, 1), uint(max(u_pbr_antitile_coarse_seed, 1)));
	vec3 fine = spat_periodic_value(
		detail_position / fine_cell,
		max(u_pbr_antitile_fine_period, 1), uint(max(u_pbr_antitile_fine_seed, 1)));

	vec3 offset_m = coarse * u_pbr_antitile_coarse_offset_m
		+ fine * u_pbr_antitile_fine_offset_m;
	return detail_position + offset_m * u_pbr_antitile_strength;
}
