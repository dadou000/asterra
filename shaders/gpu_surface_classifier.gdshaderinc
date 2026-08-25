// First-pass GPU surface classifier. Inputs are coarse baked context plus the
// actually displaced local slope. This deliberately classifies materials after
// geomorph synthesis, so cliffs expose bedrock and depositional terrain collects
// loose material coherently.

float sc_is_biome(float biome, float id) {
	return 1.0 - step(0.49, abs(biome - id));
}

vec3 sc_rock_color(float rock_id) {
	if (rock_id < 0.5) return vec3(0.335, 0.305, 0.275); // granite
	if (rock_id < 1.5) return vec3(0.085, 0.082, 0.080); // basalt
	if (rock_id < 2.5) return vec3(0.105, 0.104, 0.098); // gabbro
	if (rock_id < 3.5) return vec3(0.250, 0.240, 0.232); // gneiss
	if (rock_id < 4.5) return vec3(0.175, 0.170, 0.158); // schist
	if (rock_id < 5.5) return vec3(0.365, 0.230, 0.140); // sandstone
	if (rock_id < 6.5) return vec3(0.155, 0.146, 0.132); // shale
	if (rock_id < 7.5) return vec3(0.415, 0.400, 0.352); // limestone
	if (rock_id < 8.5) return vec3(0.375, 0.358, 0.318); // dolomite
	if (rock_id < 9.5) return vec3(0.265, 0.222, 0.180); // conglomerate
	if (rock_id < 10.5) return vec3(0.455, 0.442, 0.422); // quartzite
	if (rock_id < 11.5) return vec3(0.330, 0.298, 0.260); // rhyolitic tuff
	return vec3(0.120, 0.145, 0.118); // serpentinite
}

vec3 sc_soil_color(vec4 soil, vec4 surface) {
	float sand = soil.r;
	float silt = soil.g;
	float clay = soil.b;
	float organic = soil.a;
	vec3 sandy = vec3(0.31, 0.235, 0.135);
	vec3 silty = vec3(0.205, 0.155, 0.095);
	vec3 clayey = vec3(0.245, 0.105, 0.055);
	vec3 mineral = (sandy * sand + silty * silt + clayey * clay)
		/ max(sand + silt + clay, 1e-4);
	vec3 humus = vec3(0.055, 0.035, 0.018);
	vec3 c = mix(mineral, humus, clamp(organic * 1.35, 0.0, 0.82));
	return c * mix(1.0, 0.68, surface.g);
}

vec3 sc_vegetation_color(vec4 surface, vec4 climate, float biome) {
	float wet = surface.g;
	float cold = clamp((5.0 - asterra_ctx_temperature_c(climate)) / 18.0, 0.0, 1.0);
	vec3 green = mix(vec3(0.14, 0.19, 0.055), vec3(0.055, 0.145, 0.038), wet);
	green = mix(green, vec3(0.115, 0.120, 0.070), cold * 0.55);
	float steppe = sc_is_biome(biome, 11.0);
	float savanna = sc_is_biome(biome, 13.0);
	green = mix(green, vec3(0.205, 0.175, 0.070), max(steppe, savanna) * (1.0 - wet) * 0.75);
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

	float soil_repose = mix(42.0, 27.0, clamp(wet * 0.75 + soil.b * 0.45, 0.0, 1.0));
	float loose_stable = 1.0 - smoothstep(soil_repose - 4.0, soil_repose + 5.0, slope_deg);
	float sand_stable = 1.0 - smoothstep(29.0, 36.0, slope_deg);
	float veg_stable = 1.0 - smoothstep(32.0, 46.0, slope_deg);
	float thin = 1.0 - smoothstep(0.06, 0.55, soil_depth);
	float forced_rock = max(sc_is_biome(biome, 18.0), smoothstep(43.0, 58.0, slope_deg));
	float rock = clamp(max(thin * (1.0 - veg_potential * 0.65), forced_rock), 0.0, 1.0);
	rock = max(rock, mountain * smoothstep(48.0, 62.0, slope_deg));

	float sand = soil.r * arid * sand_stable * (1.0 - rock);
	sand *= 0.45 + sediment * 0.85;
	if (sc_is_biome(biome, 12.0) > 0.5) sand = max(sand, soil.r * 0.72 * sand_stable);

	float vegetation = veg_potential * veg_stable * loose_stable * (1.0 - rock);
	vegetation *= 1.0 - arid * 0.62;
	vegetation *= smoothstep(0.04, 0.28, soil_depth);

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

	float mud = wet * (soil.g * 0.55 + soil.b * 0.85) * (1.0 - smoothstep(12.0, 28.0, slope_deg));
	mud *= 0.35 + depositional * 0.95;
	mud = max(mud, sc_is_biome(biome, 16.0) * wet * 0.8);

	float snow = smoothstep(2.0, -8.0, temp) * (1.0 - smoothstep(28.0, 50.0, slope_deg));
	snow = max(snow, sc_is_biome(biome, 3.0));
	snow = max(snow, sc_is_biome(biome, 17.0) * smoothstep(3.0, -5.0, temp));

	float scree = mountain * smoothstep(25.0, 34.0, slope_deg)
		* (1.0 - smoothstep(45.0, 55.0, slope_deg));
	scree *= clamp(0.35 + sediment * 0.9 + geology.r * 0.35, 0.0, 1.0);

	float gravel = hydro.b * (1.0 - smoothstep(18.0, 34.0, slope_deg));
	gravel *= clamp(0.35 + sediment * 0.9, 0.0, 1.0);
	gravel = max(gravel, sc_is_biome(biome, 19.0) * 0.72);
	return clamp(vec4(mud, snow, scree, gravel), 0.0, 1.0);
}

vec3 sc_compose_albedo(vec4 primary, vec4 secondary, vec4 soil, vec4 surface,
		vec4 climate, float biome, float rock_id) {
	vec3 rock = sc_rock_color(rock_id);
	vec3 soil_c = sc_soil_color(soil, surface);
	vec3 vegetation = sc_vegetation_color(surface, climate, biome);
	vec3 sand = mix(vec3(0.40, 0.315, 0.185), vec3(0.53, 0.42, 0.245), soil.r);
	vec3 mud = soil_c * vec3(0.50, 0.46, 0.42);
	vec3 snow = vec3(0.82, 0.86, 0.89);
	vec3 scree = rock * 0.82 + vec3(0.035);
	vec3 gravel = mix(rock, vec3(0.23, 0.215, 0.195), 0.48);

	float total = dot(primary, vec4(1.0)) + dot(secondary, vec4(1.0));
	if (total < 1e-4) return soil_c;
	vec3 c = rock * primary.r + soil_c * primary.g + vegetation * primary.b + sand * primary.a;
	c += mud * secondary.r + snow * secondary.g + scree * secondary.b + gravel * secondary.a;
	return c / total;
}

float sc_compose_roughness(vec4 primary, vec4 secondary, vec4 surface) {
	float total = dot(primary, vec4(1.0)) + dot(secondary, vec4(1.0));
	if (total < 1e-4) return 0.9;
	float r = 0.76 * primary.r + 0.92 * primary.g + 0.96 * primary.b + 0.82 * primary.a;
	r += mix(0.72, 0.48, surface.g) * secondary.r;
	r += 0.92 * secondary.g + 0.86 * secondary.b + 0.80 * secondary.a;
	return clamp(r / total, 0.42, 0.98);
}
