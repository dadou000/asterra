// Material-dependent geometric microrelief for the dense near-field patch.
//
// This is actual vertex displacement, not a normal-map effect. The caller only
// evaluates it on the dedicated 1/4-L0-spacing micro patch. Features are kept at
// >= 1 m characteristic scale so the current ~0.1875 m micro spacing samples them
// safely. Sub-decimetre detail stays in PBR normals/scatters.
//
// gpu_surface_antitile.gdshaderinc and gpu_surface_classifier.gdshaderinc must be
// included first. We reuse the periodic stochastic field so floating-origin wraps
// at 4096 m never change microgeometry phase.

uniform float u_microrelief_enabled = 1.0;
uniform float u_microrelief_strength : hint_range(0.0, 2.0) = 1.0;

float mmr_scalar(vec3 v) {
	return dot(v, vec3(0.577350269, 0.577350269, 0.577350269));
}

float mmr_noise(vec3 p, float cell_m, int period, uint seed) {
	return clamp(mmr_scalar(spat_periodic_value(p / cell_m, period, seed)), -1.0, 1.0);
}

float mmr_ridge(float n) {
	return 1.0 - abs(n);
}

float mmr_rock_height(vec3 p, float rock_id, vec4 geology, vec4 structure) {
	float n4 = mmr_noise(p, 4.0, 1024, 0x9127u);
	float n2 = mmr_noise(p, 2.0, 2048, 0x53d1u);
	float n1 = mmr_noise(p, 1.0, 4096, 0xc781u);
	float fault = structure.g;
	float erodible = geology.r;
	float dip = geology.g;

	float block = (mmr_ridge(n4) - 0.52) * 0.12 + n2 * 0.055 + n1 * 0.022;
	float fracture = pow(clamp(mmr_ridge(n2), 0.0, 1.0), 7.0);
	block -= fracture * (0.035 + fault * 0.055);

	// Integer world-axis coefficients preserve exact 4096 m wrap periodicity.
	float phase_a = (p.x + 2.0 * p.y + p.z) * TAU / 4.0;
	float phase_b = (2.0 * p.x - p.y + p.z) * TAU / 2.0;
	float bedding = sin(phase_a + n4 * 1.5) * 0.5 + sin(phase_b + n2) * 0.22;
	float bed_step = (smoothstep(-0.20, 0.22, sin(phase_a + n4)) - 0.5) * 2.0;

	float h = block;
	if (rock_id < 0.5) { // granite: massive, jointed blocks
		h = block * 1.15 + n4 * 0.040;
	} else if (rock_id < 1.5) { // basalt: tighter block/joint relief
		h = block * 0.82 - fracture * 0.030;
	} else if (rock_id < 2.5) { // gabbro: coarse crystalline
		h = block * 1.08 + n1 * 0.030;
	} else if (rock_id < 3.5) { // gneiss: broad foliation
		h = block * 0.55 + bedding * 0.095 * (0.55 + dip * 0.45);
	} else if (rock_id < 4.5) { // schist: stronger planar cleavage
		h = block * 0.42 + bedding * 0.125 * (0.65 + dip * 0.50);
	} else if (rock_id < 5.5) { // sandstone: resistant beds and ledges
		h = block * 0.30 + bed_step * 0.105 + bedding * 0.035;
	} else if (rock_id < 6.5) { // shale: thin weak beds
		h = block * 0.20 + bed_step * 0.055 + bedding * 0.030;
	} else if (rock_id < 7.5) { // limestone: jointed beds
		h = block * 0.52 + bed_step * 0.075 - fracture * 0.030;
	} else if (rock_id < 8.5) { // dolomite
		h = block * 0.58 + bed_step * 0.065;
	} else if (rock_id < 9.5) { // conglomerate: coarse irregular clasts
		float clast = pow(clamp(mmr_ridge(n1), 0.0, 1.0), 3.0) - 0.45;
		h = block * 0.55 + clast * 0.095;
	} else if (rock_id < 10.5) { // quartzite: hard, massive, sharp
		h = block * 1.22 + mmr_ridge(n2) * 0.050 - 0.025;
	} else if (rock_id < 11.5) { // rhyolitic tuff: porous/crumbly
		h = block * 0.62 + n1 * 0.045 - pow(clamp(mmr_ridge(n1), 0.0, 1.0), 6.0) * 0.025;
	} else { // serpentinite: slick irregular veining
		h = block * 0.48 + bedding * 0.070 + n2 * 0.035;
	}

	// Softer/weathered families lose relief; active faulting adds fresh fractures.
	return h * mix(1.08, 0.72, clamp(erodible, 0.0, 1.0)) * (1.0 + fault * 0.16);
}

float mmr_height(vec3 detail_position, float rock_id, float biome,
		vec4 soil, vec4 surface, vec4 geology, vec4 structure,
		vec4 climate, vec4 hydro, vec4 landform) {
	if (u_microrelief_enabled < 0.5) return 0.0;

	float soil_depth = asterra_ctx_soil_depth_m(surface);
	float temp_c = asterra_ctx_temperature_c(climate);
	float thin = 1.0 - smoothstep(0.08, 0.62, soil_depth);
	float bare_biome = sc_is_biome(biome, 18.0);
	float alpine = sc_is_biome(biome, 17.0);
	float mountain = landform.r;
	float arid = landform.g;
	float depositional = landform.a;

	// Vertex-stage proxy. Final visible material classification still happens in
	// fragment() from the actual displaced slope; this only chooses which physical
	// type of relief should exist at sub-metre topology scale.
	float rock_w = clamp(max(bare_biome, thin * (0.45 + mountain * 0.65))
		* (1.0 - surface.b * 0.45), 0.0, 1.0);
	rock_w = max(rock_w, alpine * thin * 0.72);
	float sand_w = soil.r * arid * (1.0 - rock_w) * (0.45 + surface.a * 0.55);
	float mud_w = surface.g * (soil.g * 0.45 + soil.b * 0.80)
		* depositional * (1.0 - rock_w);
	float cold = 1.0 - smoothstep(-7.0, 2.0, temp_c);
	float snow_w = cold * (0.35 + alpine * 0.65) * (1.0 - rock_w * 0.50);
	float gravel_w = hydro.b * (0.30 + surface.a * 0.70) * (1.0 - mud_w);
	float scree_w = mountain * surface.a * thin * (1.0 - rock_w * 0.35);

	float loose_total = sand_w + mud_w + snow_w + gravel_w + scree_w;
	float soil_w = clamp((1.0 - rock_w) * (1.0 - min(loose_total, 1.0)), 0.0, 1.0);

	float n8 = mmr_noise(detail_position, 8.0, 512, 0x2db7u);
	float n4 = mmr_noise(detail_position, 4.0, 1024, 0x7a31u);
	float n2 = mmr_noise(detail_position, 2.0, 2048, 0xb921u);
	float n1 = mmr_noise(detail_position, 1.0, 4096, 0xe173u);

	float rock_h = mmr_rock_height(detail_position, rock_id, geology, structure);
	float soil_h = n2 * 0.018 + n1 * 0.008;

	// Sand is low-amplitude at this topology scale; true centimetre ripples remain
	// a normal/displacement-texture concern until a finer nested patch exists.
	float sand_ridge = mmr_ridge(n2);
	float sand_h = (sand_ridge - 0.52) * 0.045 + n4 * 0.012;

	float mud_h = n2 * 0.018 + n1 * 0.008 - pow(clamp(mmr_ridge(n1), 0.0, 1.0), 7.0) * 0.012;
	float snow_h = n4 * 0.035 + n2 * 0.018;
	float gravel_h = (pow(clamp(mmr_ridge(n1), 0.0, 1.0), 3.0) - 0.42) * 0.070 + n2 * 0.018;
	float scree_h = (pow(clamp(mmr_ridge(n2), 0.0, 1.0), 2.2) - 0.45) * 0.105 + n4 * 0.025;

	float weighted = rock_h * rock_w
		+ soil_h * soil_w
		+ sand_h * sand_w
		+ mud_h * mud_w
		+ snow_h * snow_w
		+ gravel_h * gravel_w
		+ scree_h * scree_w;
	float total = max(rock_w + soil_w + sand_w + mud_w + snow_w + gravel_w + scree_w, 1.0);

	// Very broad sub-metre ground undulation prevents the material-specific terms
	// from looking like independent embossed masks.
	weighted = weighted / total + n8 * 0.010;
	return weighted * u_microrelief_strength;
}
