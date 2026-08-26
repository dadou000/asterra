// Low-cost stochastic de-periodisation for scanned terrain materials.
//
// Full stochastic texture bombing would multiply the already expensive
// triplanar albedo/roughness/normal sample count. Instead we continuously warp
// the shared precision-safe detail position with periodic vector value noise.
// That breaks long straight repetitions and repeated landmark alignment without
// adding any texture fetches.
//
// Both lattice periods divide the 4096 m floating-origin detail wrap exactly.
// Therefore a rebase changes the input by an integer noise period and cannot
// cause the material phase to jump.
//
// gpu_geomorph.gdshaderinc must be included before this file; we reuse gm_hash().

uniform float u_pbr_antitile_strength : hint_range(0.0, 1.5) = 1.0;

const float SPAT_WRAP_M = 4096.0;
const float SPAT_COARSE_CELL_M = 32.0;
const float SPAT_FINE_CELL_M = 8.0;
const int SPAT_COARSE_PERIOD = 128; // 4096 / 32
const int SPAT_FINE_PERIOD = 512;   // 4096 / 8

int spat_posmod(int value, int period) {
	int m = value % period;
	return m < 0 ? m + period : m;
}

ivec3 spat_wrap_cell(ivec3 cell, int period) {
	return ivec3(
		spat_posmod(cell.x, period),
		spat_posmod(cell.y, period),
		spat_posmod(cell.z, period));
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

	vec3 coarse = spat_periodic_value(
		detail_position / SPAT_COARSE_CELL_M,
		SPAT_COARSE_PERIOD, 0x71a5u);
	vec3 fine = spat_periodic_value(
		detail_position / SPAT_FINE_CELL_M,
		SPAT_FINE_PERIOD, 0xb43du);

	// The coarse field moves the phase by a substantial fraction of a 1-2 m scan
	// tile while changing only slowly in world space. The smaller field prevents
	// the resulting bend from becoming recognisable at walking distance.
	vec3 offset_m = coarse * 0.58 + fine * 0.14;
	return detail_position + offset_m * u_pbr_antitile_strength;
}
