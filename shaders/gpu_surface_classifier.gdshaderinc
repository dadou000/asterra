// GPU surface classifier. Inputs are continuous baked context plus the actually
// displaced local slope. Biome IDs remain available for diagnostics, but visible
// material transitions are driven by continuous climate/soil/hydrology fields so
// coarse biome-cell boundaries never become hard surface-colour borders.

// Deprecated for visible rendering. Keeping the helper allows older optional
// modules to compile while preventing nearest-filtered biome IDs from reintroducing
// step boundaries downstream (forest PBR, grass stature, etc.).
float sc_is_biome(float biome, float id) {
	return 0.0;
}

vec3 sc_rock_color(float rock_id) {
	if (rock_id < 0.5) return vec3(0.335, 0.305, 0.275);
	if (rock_id < 1.5) return vec3(0.085, 0.082, 0.080);
	if (rock_id < 2.5) return vec3(0.105, 0.104, 0.098);
	if (rock_id < 3.5) return vec3(0.250, 0.240, 0.232);
	if (rock_id < 4.5) return vec3(0.175, 0.170, 0.158);
	if (rock_id < 5.5) return vec3(0.365, 0.230, 0.140);
	if (rock_id < 6.5) return vec3(0.155, 0.146, 0.132);
	if (rock_id < 7.5) return vec3(0.415, 0.400, 0.352);
	if (rock_id < 8.5) return vec3(0.375, 0.358, 0.318);
	if (rock_id < 9.5) return vec3(0.265, 0.222, 0.180);
	if (rock_id < 10.5) return vec3(0.455, 0.442, 0.422);
	if (rock_id < 11.5) return vec3(0.330, 0.298, 0.260);
	return vec3(0.120, 0.145, 0.118);
}

vec3 sc_soil_color(vec4 soil, vec4 surface) {
	float sand = soil.r;
	float silt = soil.g;
	float clay = soil.b;
	float organic = soil.a;
	// Richer mineral colours than the original neutral palette. These still remain
	// physically dark in linear space, but retain enough chroma to survive orbital
	// atmospheric attenuation.
	vec3 sandy = vec3(0.385, 0.225, 0.075);
	vec3 silty = vec3(0.220, 0.120, 0.040);
	vec3 clayey = vec3(0.315, 0.075, 0.028);
	vec3 mineral = (sandy * sand + silty * silt + clayey * clay)
		/ max(sand + silt + clay, 1e-4);
	vec3 humus = vec3(0.028, 0.017, 0.006);
	vec3 c = mix(mineral, humus, clamp(organic * 1.35, 0.0, 0.84));
	return c * mix(1.0, 0.62, surface.g);
}

vec3 sc_vegetation_color(vec4 surface, vec4 climate, float biome) {
	float wet = surface.g;
	float biomass = surface.b;
	float temp = asterra_ctx_temperature_c(climate);
	float precip = asterra_ctx_precip_mm(climate);
	float cold = clamp((5.0 - temp) / 18.0, 0.0, 1.0);
	float dry = clamp((1.0 - wet) * (1.0 - precip / 1100.0), 0.0, 1.0);

	// Grassland stays comparatively bright, while sustained wet/biomass-rich
	// vegetation collapses toward a genuinely deep forest green. This is continuous
	// climate-driven colour, so it does not create categorical biome seams.
	vec3 green = mix(vec3(0.185, 0.255, 0.042), vec3(0.030, 0.155, 0.030), wet);
	float lush = smoothstep(450.0, 1350.0, precip)
		* smoothstep(0.26, 0.76, biomass)
		* smoothstep(0.22, 0.72, wet);
	green = mix(green, vec3(0.010, 0.075, 0.016), lush * 0.92);
	green = mix(green, vec3(0.095, 0.105, 0.045), cold * 0.55);
	green = mix(green, vec3(0.285, 0.175, 0.030), dry * 0.82);
	return green;
}

vec4 sc_primary_weights(vec4 soil, vec4 surface, vec4 geology, vec4 structure,
		vec4 climate, vec4 hydro, vec4 landform, float biome, float slope_deg) {
	float soil_depth = asterra_ctx_soil_depth_m(surface);
	float wet = surface.g;
	float veg_potential = surface.b;
	float sediment = surface.a;
	float mountain = landform.r;
	float arid = landform.g;
	float precip = asterra_ctx_precip_mm(climate);
	float temp = asterra_ctx_temperature_c(climate);

	float soil_repose = mix(42.0, 27.0, clamp(wet * 0.75 + soil.b * 0.45, 0.0, 1.0));
	float loose_stable = 1.0 - smoothstep(soil_repose - 4.0, soil_repose + 5.0, slope_deg);
	float sand_stable = 1.0 - smoothstep(29.0, 36.0, slope_deg);
	float veg_stable = 1.0 - smoothstep(32.0, 46.0, slope_deg);
	float thin = 1.0 - smoothstep(0.06, 0.55, soil_depth);
	float climatic_bare = smoothstep(0.58, 0.94, arid)
		* (1.0 - smoothstep(220.0, 650.0, precip));
	climatic_bare = max(climatic_bare, 1.0 - smoothstep(-14.0, -3.0, temp));
	float forced_rock = max(smoothstep(43.0, 58.0, slope_deg), thin * climatic_bare * 0.72);
	float rock = clamp(max(thin * (1.0 - veg_potential * 0.65), forced_rock), 0.0, 1.0);
	rock = max(rock, mountain * smoothstep(48.0, 62.0, slope_deg));

	float sand = soil.r * arid * sand_stable * (1.0 - rock);
	sand *= 0.45 + sediment * 0.85;
	float dune_climate = smoothstep(0.55, 0.92, arid)
		* (1.0 - smoothstep(300.0, 850.0, precip));
	sand = max(sand, soil.r * dune_climate * 0.72 * sand_stable * (1.0 - rock));

	float thermal_growth = smoothstep(-7.0, 4.0, temp);
	float vegetation = veg_potential * veg_stable * loose_stable * (1.0 - rock);
	vegetation *= 1.0 - arid * 0.62;
	vegetation *= smoothstep(0.04, 0.28, soil_depth) * thermal_growth;

	float mineral_soil = loose_stable * (1.0 - rock) * (1.0 - sand * 0.72);
	mineral_soil *= smoothstep(0.025, 0.22, soil_depth);
	return clamp(vec4(rock, mineral_soil, vegetation, sand), 0.0, 1.0);
}

vec4 sc_secondary_weights(vec4 soil, vec4 surface, vec4 geology, vec4 structure,
		vec4 climate, vec4 hydro, vec4 landform, float biome, float slope_deg) {
	float wet = surface.g;
	float sediment = surface.a;
	float mountain = landform.r;
	float depositional = landform.a;
	float temp = asterra_ctx_temperature_c(climate);
	float precip = asterra_ctx_precip_mm(climate);

	float mud = wet * (soil.g * 0.55 + soil.b * 0.85) * (1.0 - smoothstep(12.0, 28.0, slope_deg));
	mud *= 0.35 + depositional * 0.95;
	float saturated = smoothstep(0.68, 0.92, wet) * smoothstep(350.0, 900.0, precip);
	mud = max(mud, saturated * (soil.g * 0.35 + soil.b * 0.65) * 0.72);

	float cold = 1.0 - smoothstep(-8.0, 2.0, temp);
	float snow = cold * (1.0 - smoothstep(28.0, 50.0, slope_deg));
	float marginal_snow = 1.0 - smoothstep(-2.0, 5.0, temp);
	snow = max(snow, marginal_snow * smoothstep(500.0, 1400.0, precip) * 0.62);

	float scree = mountain * smoothstep(25.0, 34.0, slope_deg)
		* (1.0 - smoothstep(45.0, 55.0, slope_deg));
	scree *= clamp(0.35 + sediment * 0.9 + geology.r * 0.35, 0.0, 1.0);

	float gravel = hydro.b * (1.0 - smoothstep(18.0, 34.0, slope_deg));
	gravel *= clamp(0.35 + sediment * 0.9, 0.0, 1.0);
	gravel = max(gravel, hydro.a * sediment * 0.55);
	return clamp(vec4(mud, snow, scree, gravel), 0.0, 1.0);
}

vec3 sc_grade_albedo(vec3 c) {
	float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
	// Stronger chroma plus a mild linear-space S-like contrast expansion. Keeping
	// the pivot low is important because physically plausible terrain albedos live
	// far below display-space middle grey.
	c = mix(vec3(luma), c, 1.24);
	c = vec3(0.115) + (c - vec3(0.115)) * 1.10;
	return clamp(c, vec3(0.004), vec3(0.92));
}

vec3 sc_compose_albedo(vec4 primary, vec4 secondary, vec4 soil, vec4 surface,
		vec4 climate, float biome, float rock_id) {
	vec3 rock = sc_rock_color(rock_id);
	vec3 soil_c = sc_soil_color(soil, surface);
	vec3 vegetation = sc_vegetation_color(surface, climate, biome);
	vec3 sand = mix(vec3(0.485, 0.285, 0.095), vec3(0.660, 0.445, 0.155), soil.r);
	vec3 mud = soil_c * vec3(0.42, 0.36, 0.30);
	vec3 snow = vec3(0.84, 0.89, 0.94);
	vec3 scree = rock * 0.80 + vec3(0.026);
	vec3 gravel = mix(rock, vec3(0.205, 0.185, 0.155), 0.44);

	float total = dot(primary, vec4(1.0)) + dot(secondary, vec4(1.0));
	if (total < 1e-4) return sc_grade_albedo(soil_c);
	vec3 c = rock * primary.r + soil_c * primary.g + vegetation * primary.b + sand * primary.a;
	c += mud * secondary.r + snow * secondary.g + scree * secondary.b + gravel * secondary.a;
	return sc_grade_albedo(c / total);
}

float sc_compose_roughness(vec4 primary, vec4 secondary, vec4 surface) {
	float total = dot(primary, vec4(1.0)) + dot(secondary, vec4(1.0));
	if (total < 1e-4) return 0.9;
	float r = 0.76 * primary.r + 0.92 * primary.g + 0.96 * primary.b + 0.82 * primary.a;
	r += mix(0.72, 0.48, surface.g) * secondary.r;
	r += 0.92 * secondary.g + 0.86 * secondary.b + 0.80 * secondary.a;
	return clamp(r / total, 0.42, 0.98);
}