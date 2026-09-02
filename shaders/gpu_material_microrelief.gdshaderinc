// Material-dependent geometric microrelief for the dense near-field patch.
// Visible relief uses continuous physical context; biome IDs are retained only in
// the interface for compatibility/debug and never create hard geometric borders.
// Phase 33 exposes the final per-material relief contributions while retaining the
// historical values as 1.0 multipliers.

uniform float u_microrelief_enabled = 1.0;
uniform float u_microrelief_strength : hint_range(0.0, 2.0) = 1.0;
uniform float u_microrelief_rock_scale : hint_range(0.0, 4.0) = 1.0;
uniform float u_microrelief_soil_scale : hint_range(0.0, 4.0) = 1.0;
uniform float u_microrelief_sand_scale : hint_range(0.0, 4.0) = 1.0;
uniform float u_microrelief_mud_scale : hint_range(0.0, 4.0) = 1.0;
uniform float u_microrelief_snow_scale : hint_range(0.0, 4.0) = 1.0;
uniform float u_microrelief_gravel_scale : hint_range(0.0, 4.0) = 1.0;
uniform float u_microrelief_scree_scale : hint_range(0.0, 4.0) = 1.0;
uniform float u_microrelief_base_noise_scale : hint_range(0.0, 4.0) = 1.0;

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

	float phase_a = (p.x + 2.0 * p.y + p.z) * TAU / 4.0;
	float phase_b = (2.0 * p.x - p.y + p.z) * TAU / 2.0;
	float bedding = sin(phase_a + n4 * 1.5) * 0.5 + sin(phase_b + n2) * 0.22;
	float bed_step = (smoothstep(-0.20, 0.22, sin(phase_a + n4)) - 0.5) * 2.0;

	float h = block;
	if (rock_id < 0.5) {
		h = block * 1.15 + n4 * 0.040;
	} else if (rock_id < 1.5) {
		h = block * 0.82 - fracture * 0.030;
	} else if (rock_id < 2.5) {
		h = block * 1.08 + n1 * 0.030;
	} else if (rock_id < 3.5) {
		h = block * 0.55 + bedding * 0.095 * (0.55 + dip * 0.45);
	} else if (rock_id < 4.5) {
		h = block * 0.42 + bedding * 0.125 * (0.65 + dip * 0.50);
	} else if (rock_id < 5.5) {
		h = block * 0.30 + bed_step * 0.105 + bedding * 0.035;
	} else if (rock_id < 6.5) {
		h = block * 0.20 + bed_step * 0.055 + bedding * 0.030;
	} else if (rock_id < 7.5) {
		h = block * 0.52 + bed_step * 0.075 - fracture * 0.030;
	} else if (rock_id < 8.5) {
		h = block * 0.58 + bed_step * 0.065;
	} else if (rock_id < 9.5) {
		float clast = pow(clamp(mmr_ridge(n1), 0.0, 1.0), 3.0) - 0.45;
		h = block * 0.55 + clast * 0.095;
	} else if (rock_id < 10.5) {
		h = block * 1.22 + mmr_ridge(n2) * 0.050 - 0.025;
	} else if (rock_id < 11.5) {
		h = block * 0.62 + n1 * 0.045 - pow(clamp(mmr_ridge(n1), 0.0, 1.0), 6.0) * 0.025;
	} else {
		h = block * 0.48 + bedding * 0.070 + n2 * 0.035;
	}
	return h * mix(1.08, 0.72, clamp(erodible, 0.0, 1.0)) * (1.0 + fault * 0.16);
}

float mmr_height(vec3 detail_position, float rock_id, float biome,
		vec4 soil, vec4 surface, vec4 geology, vec4 structure,
		vec4 climate, vec4 hydro, vec4 landform) {
	if (u_microrelief_enabled < 0.5) return 0.0;

	float soil_depth = asterra_ctx_soil_depth_m(surface);
	float temp_c = asterra_ctx_temperature_c(climate);
	float precip = asterra_ctx_precip_mm(climate);
	float thin = 1.0 - smoothstep(0.08, 0.62, soil_depth);
	float mountain = landform.r;
	float arid = landform.g;
	float depositional = landform.a;
	float cold = 1.0 - smoothstep(-7.0, 2.0, temp_c);
	float climatic_bare = smoothstep(0.58, 0.94, arid)
		* (1.0 - smoothstep(240.0, 720.0, precip));
	climatic_bare = max(climatic_bare, 1.0 - smoothstep(-15.0, -4.0, temp_c));
	float alpine_cold = mountain * (1.0 - smoothstep(-3.0, 5.0, temp_c));

	float rock_w = clamp(max(thin * (0.45 + mountain * 0.65), climatic_bare * thin * 0.72)
		* (1.0 - surface.b * 0.45), 0.0, 1.0);
	rock_w = max(rock_w, alpine_cold * thin * 0.72);
	float sand_w = soil.r * arid * (1.0 - rock_w) * (0.45 + surface.a * 0.55);
	float mud_w = surface.g * (soil.g * 0.45 + soil.b * 0.80)
		* depositional * (1.0 - rock_w);
	float snow_w = cold * (0.35 + alpine_cold * 0.65) * (1.0 - rock_w * 0.50);
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
	float sand_h = (mmr_ridge(n2) - 0.52) * 0.045 + n4 * 0.012;
	float mud_h = n2 * 0.018 + n1 * 0.008 - pow(clamp(mmr_ridge(n1), 0.0, 1.0), 7.0) * 0.012;
	float snow_h = n4 * 0.035 + n2 * 0.018;
	float gravel_h = (pow(clamp(mmr_ridge(n1), 0.0, 1.0), 3.0) - 0.42) * 0.070 + n2 * 0.018;
	float scree_h = (pow(clamp(mmr_ridge(n2), 0.0, 1.0), 2.2) - 0.45) * 0.105 + n4 * 0.025;

	float weighted = rock_h * rock_w * u_microrelief_rock_scale
		+ soil_h * soil_w * u_microrelief_soil_scale
		+ sand_h * sand_w * u_microrelief_sand_scale
		+ mud_h * mud_w * u_microrelief_mud_scale
		+ snow_h * snow_w * u_microrelief_snow_scale
		+ gravel_h * gravel_w * u_microrelief_gravel_scale
		+ scree_h * scree_w * u_microrelief_scree_scale;
	float total = max(rock_w + soil_w + sand_w + mud_w + snow_w + gravel_w + scree_w, 1.0);
	weighted = weighted / total + n8 * 0.010 * u_microrelief_base_noise_scale;
	return weighted * u_microrelief_strength;
}
