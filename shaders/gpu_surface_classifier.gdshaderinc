// GPU surface classifier. Inputs are continuous baked context plus the actually
// displaced local slope. Biome IDs remain diagnostic only; visible transitions
// are continuous. Production thresholds, material palette and roughness targets
// are graph-owned uniforms; defaults exactly reproduce the historical renderer.

uniform vec4 u_classifier_primary_scale = vec4(1.0);
uniform vec4 u_classifier_secondary_scale = vec4(1.0);
uniform float u_classifier_albedo_chroma : hint_range(0.0, 3.0) = 1.24;
uniform float u_classifier_albedo_contrast : hint_range(0.0, 3.0) = 1.10;
uniform float u_classifier_albedo_pivot : hint_range(0.0, 1.0) = 0.115;
uniform float u_classifier_roughness_scale : hint_range(0.0, 3.0) = 1.0;
uniform float u_classifier_roughness_bias : hint_range(-1.0, 1.0) = 0.0;
uniform float u_classifier_roughness_min : hint_range(0.0, 1.0) = 0.42;
uniform float u_classifier_roughness_max : hint_range(0.0, 1.0) = 0.98;

// Physical / climatic selection thresholds.
uniform float u_sc_soil_repose_dry_deg = 42.0;
uniform float u_sc_soil_repose_wet_deg = 27.0;
uniform float u_sc_loose_margin_low_deg = 4.0;
uniform float u_sc_loose_margin_high_deg = 5.0;
uniform float u_sc_sand_slope_start_deg = 29.0;
uniform float u_sc_sand_slope_end_deg = 36.0;
uniform float u_sc_vegetation_slope_start_deg = 32.0;
uniform float u_sc_vegetation_slope_end_deg = 46.0;
uniform float u_sc_thin_soil_start_m = 0.06;
uniform float u_sc_thin_soil_end_m = 0.55;
uniform float u_sc_rock_slope_start_deg = 43.0;
uniform float u_sc_rock_slope_end_deg = 58.0;
uniform float u_sc_mountain_rock_slope_start_deg = 48.0;
uniform float u_sc_mountain_rock_slope_end_deg = 62.0;
uniform float u_sc_arid_bare_start = 0.58;
uniform float u_sc_arid_bare_end = 0.94;
uniform float u_sc_bare_precip_start_mm = 220.0;
uniform float u_sc_bare_precip_end_mm = 650.0;
uniform float u_sc_bare_temp_start_c = -14.0;
uniform float u_sc_bare_temp_end_c = -3.0;
uniform float u_sc_dune_arid_start = 0.55;
uniform float u_sc_dune_arid_end = 0.92;
uniform float u_sc_dune_precip_start_mm = 300.0;
uniform float u_sc_dune_precip_end_mm = 850.0;
uniform float u_sc_thermal_growth_start_c = -7.0;
uniform float u_sc_thermal_growth_end_c = 4.0;
uniform float u_sc_vegetation_depth_start_m = 0.04;
uniform float u_sc_vegetation_depth_end_m = 0.28;
uniform float u_sc_soil_depth_start_m = 0.025;
uniform float u_sc_soil_depth_end_m = 0.22;
uniform float u_sc_mud_slope_start_deg = 12.0;
uniform float u_sc_mud_slope_end_deg = 28.0;
uniform float u_sc_saturated_wet_start = 0.68;
uniform float u_sc_saturated_wet_end = 0.92;
uniform float u_sc_saturated_precip_start_mm = 350.0;
uniform float u_sc_saturated_precip_end_mm = 900.0;
uniform float u_sc_snow_temp_start_c = -8.0;
uniform float u_sc_snow_temp_end_c = 2.0;
uniform float u_sc_snow_slope_start_deg = 28.0;
uniform float u_sc_snow_slope_end_deg = 50.0;
uniform float u_sc_marginal_snow_temp_start_c = -2.0;
uniform float u_sc_marginal_snow_temp_end_c = 5.0;
uniform float u_sc_snow_precip_start_mm = 500.0;
uniform float u_sc_snow_precip_end_mm = 1400.0;
uniform float u_sc_scree_slope_start_deg = 25.0;
uniform float u_sc_scree_slope_full_deg = 34.0;
uniform float u_sc_scree_slope_fade_deg = 45.0;
uniform float u_sc_scree_slope_end_deg = 55.0;
uniform float u_sc_gravel_slope_start_deg = 18.0;
uniform float u_sc_gravel_slope_end_deg = 34.0;

// Exact production material palette.
uniform vec3 u_sc_rock_granite = vec3(0.335, 0.305, 0.275);
uniform vec3 u_sc_rock_basalt = vec3(0.085, 0.082, 0.080);
uniform vec3 u_sc_rock_gabbro = vec3(0.105, 0.104, 0.098);
uniform vec3 u_sc_rock_gneiss = vec3(0.250, 0.240, 0.232);
uniform vec3 u_sc_rock_schist = vec3(0.175, 0.170, 0.158);
uniform vec3 u_sc_rock_sandstone = vec3(0.365, 0.230, 0.140);
uniform vec3 u_sc_rock_shale = vec3(0.155, 0.146, 0.132);
uniform vec3 u_sc_rock_limestone = vec3(0.415, 0.400, 0.352);
uniform vec3 u_sc_rock_dolomite = vec3(0.375, 0.358, 0.318);
uniform vec3 u_sc_rock_conglomerate = vec3(0.265, 0.222, 0.180);
uniform vec3 u_sc_rock_quartzite = vec3(0.455, 0.442, 0.422);
uniform vec3 u_sc_rock_tuff = vec3(0.330, 0.298, 0.260);
uniform vec3 u_sc_rock_serpentinite = vec3(0.120, 0.145, 0.118);
uniform vec3 u_sc_soil_sandy = vec3(0.385, 0.225, 0.075);
uniform vec3 u_sc_soil_silty = vec3(0.220, 0.120, 0.040);
uniform vec3 u_sc_soil_clayey = vec3(0.315, 0.075, 0.028);
uniform vec3 u_sc_soil_humus = vec3(0.028, 0.017, 0.006);
uniform vec3 u_sc_vegetation_dry = vec3(0.285, 0.175, 0.030);
uniform vec3 u_sc_vegetation_grass = vec3(0.185, 0.255, 0.042);
uniform vec3 u_sc_vegetation_wet = vec3(0.030, 0.155, 0.030);
uniform vec3 u_sc_vegetation_lush = vec3(0.010, 0.075, 0.016);
uniform vec3 u_sc_vegetation_cold = vec3(0.095, 0.105, 0.045);
uniform vec3 u_sc_sand_low = vec3(0.485, 0.285, 0.095);
uniform vec3 u_sc_sand_high = vec3(0.660, 0.445, 0.155);
uniform vec3 u_sc_mud_tint = vec3(0.42, 0.36, 0.30);
uniform vec3 u_sc_snow_color = vec3(0.84, 0.89, 0.94);
uniform vec3 u_sc_gravel_color = vec3(0.205, 0.185, 0.155);
uniform float u_sc_scree_rock_mix = 0.80;
uniform float u_sc_scree_add = 0.026;
uniform float u_sc_gravel_rock_mix = 0.44;
uniform float u_sc_roughness_fallback = 0.90;
uniform float u_sc_roughness_rock = 0.76;
uniform float u_sc_roughness_soil = 0.92;
uniform float u_sc_roughness_vegetation = 0.96;
uniform float u_sc_roughness_sand = 0.82;
uniform float u_sc_roughness_mud_dry = 0.72;
uniform float u_sc_roughness_mud_wet = 0.48;
uniform float u_sc_roughness_snow = 0.92;
uniform float u_sc_roughness_scree = 0.86;
uniform float u_sc_roughness_gravel = 0.80;

float sc_range(float edge_a, float edge_b, float value) {
	float lo = min(edge_a, edge_b);
	float hi = max(edge_a, edge_b);
	return smoothstep(lo, max(hi, lo + 0.000001), value);
}

float sc_is_biome(float biome, float id) {
	return 0.0;
}

vec3 sc_rock_color(float rock_id) {
	if (rock_id < 0.5) return u_sc_rock_granite;
	if (rock_id < 1.5) return u_sc_rock_basalt;
	if (rock_id < 2.5) return u_sc_rock_gabbro;
	if (rock_id < 3.5) return u_sc_rock_gneiss;
	if (rock_id < 4.5) return u_sc_rock_schist;
	if (rock_id < 5.5) return u_sc_rock_sandstone;
	if (rock_id < 6.5) return u_sc_rock_shale;
	if (rock_id < 7.5) return u_sc_rock_limestone;
	if (rock_id < 8.5) return u_sc_rock_dolomite;
	if (rock_id < 9.5) return u_sc_rock_conglomerate;
	if (rock_id < 10.5) return u_sc_rock_quartzite;
	if (rock_id < 11.5) return u_sc_rock_tuff;
	return u_sc_rock_serpentinite;
}

vec3 sc_soil_color(vec4 soil, vec4 surface) {
	float sand = soil.r;
	float silt = soil.g;
	float clay = soil.b;
	float organic = soil.a;
	vec3 mineral = (u_sc_soil_sandy * sand + u_sc_soil_silty * silt + u_sc_soil_clayey * clay)
		/ max(sand + silt + clay, 1e-4);
	vec3 c = mix(mineral, u_sc_soil_humus, clamp(organic * 1.35, 0.0, 0.84));
	return c * mix(1.0, 0.62, surface.g);
}

vec3 sc_vegetation_color(vec4 surface, vec4 climate, float biome) {
	float wet = surface.g;
	float biomass = surface.b;
	float temp = asterra_ctx_temperature_c(climate);
	float precip = asterra_ctx_precip_mm(climate);
	float cold = clamp((5.0 - temp) / 18.0, 0.0, 1.0);
	float dry = clamp((1.0 - wet) * (1.0 - precip / 1100.0), 0.0, 1.0);
	vec3 green = mix(u_sc_vegetation_grass, u_sc_vegetation_wet, wet);
	float lush = smoothstep(450.0, 1350.0, precip)
		* smoothstep(0.26, 0.76, biomass)
		* smoothstep(0.22, 0.72, wet);
	green = mix(green, u_sc_vegetation_lush, lush * 0.92);
	green = mix(green, u_sc_vegetation_cold, cold * 0.55);
	green = mix(green, u_sc_vegetation_dry, dry * 0.82);
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

	float soil_repose = mix(u_sc_soil_repose_dry_deg, u_sc_soil_repose_wet_deg,
		clamp(wet * 0.75 + soil.b * 0.45, 0.0, 1.0));
	float loose_stable = 1.0 - sc_range(soil_repose - u_sc_loose_margin_low_deg,
		soil_repose + u_sc_loose_margin_high_deg, slope_deg);
	float sand_stable = 1.0 - sc_range(u_sc_sand_slope_start_deg, u_sc_sand_slope_end_deg, slope_deg);
	float veg_stable = 1.0 - sc_range(u_sc_vegetation_slope_start_deg, u_sc_vegetation_slope_end_deg, slope_deg);
	float thin = 1.0 - sc_range(u_sc_thin_soil_start_m, u_sc_thin_soil_end_m, soil_depth);
	float climatic_bare = sc_range(u_sc_arid_bare_start, u_sc_arid_bare_end, arid)
		* (1.0 - sc_range(u_sc_bare_precip_start_mm, u_sc_bare_precip_end_mm, precip));
	climatic_bare = max(climatic_bare,
		1.0 - sc_range(u_sc_bare_temp_start_c, u_sc_bare_temp_end_c, temp));
	float forced_rock = max(sc_range(u_sc_rock_slope_start_deg, u_sc_rock_slope_end_deg, slope_deg),
		thin * climatic_bare * 0.72);
	float rock = clamp(max(thin * (1.0 - veg_potential * 0.65), forced_rock), 0.0, 1.0);
	rock = max(rock, mountain * sc_range(u_sc_mountain_rock_slope_start_deg,
		u_sc_mountain_rock_slope_end_deg, slope_deg));

	float sand = soil.r * arid * sand_stable * (1.0 - rock);
	sand *= 0.45 + sediment * 0.85;
	float dune_climate = sc_range(u_sc_dune_arid_start, u_sc_dune_arid_end, arid)
		* (1.0 - sc_range(u_sc_dune_precip_start_mm, u_sc_dune_precip_end_mm, precip));
	sand = max(sand, soil.r * dune_climate * 0.72 * sand_stable * (1.0 - rock));

	float thermal_growth = sc_range(u_sc_thermal_growth_start_c, u_sc_thermal_growth_end_c, temp);
	float vegetation = veg_potential * veg_stable * loose_stable * (1.0 - rock);
	vegetation *= 1.0 - arid * 0.62;
	vegetation *= sc_range(u_sc_vegetation_depth_start_m, u_sc_vegetation_depth_end_m, soil_depth)
		* thermal_growth;

	float mineral_soil = loose_stable * (1.0 - rock) * (1.0 - sand * 0.72);
	mineral_soil *= sc_range(u_sc_soil_depth_start_m, u_sc_soil_depth_end_m, soil_depth);
	return clamp(vec4(rock, mineral_soil, vegetation, sand) * u_classifier_primary_scale,
		vec4(0.0), vec4(1.0));
}

vec4 sc_secondary_weights(vec4 soil, vec4 surface, vec4 geology, vec4 structure,
		vec4 climate, vec4 hydro, vec4 landform, float biome, float slope_deg) {
	float wet = surface.g;
	float sediment = surface.a;
	float mountain = landform.r;
	float depositional = landform.a;
	float temp = asterra_ctx_temperature_c(climate);
	float precip = asterra_ctx_precip_mm(climate);

	float mud = wet * (soil.g * 0.55 + soil.b * 0.85)
		* (1.0 - sc_range(u_sc_mud_slope_start_deg, u_sc_mud_slope_end_deg, slope_deg));
	mud *= 0.35 + depositional * 0.95;
	float saturated = sc_range(u_sc_saturated_wet_start, u_sc_saturated_wet_end, wet)
		* sc_range(u_sc_saturated_precip_start_mm, u_sc_saturated_precip_end_mm, precip);
	mud = max(mud, saturated * (soil.g * 0.35 + soil.b * 0.65) * 0.72);

	float cold = 1.0 - sc_range(u_sc_snow_temp_start_c, u_sc_snow_temp_end_c, temp);
	float snow = cold * (1.0 - sc_range(u_sc_snow_slope_start_deg, u_sc_snow_slope_end_deg, slope_deg));
	float marginal_snow = 1.0 - sc_range(u_sc_marginal_snow_temp_start_c,
		u_sc_marginal_snow_temp_end_c, temp);
	snow = max(snow, marginal_snow
		* sc_range(u_sc_snow_precip_start_mm, u_sc_snow_precip_end_mm, precip) * 0.62);

	float scree = mountain * sc_range(u_sc_scree_slope_start_deg, u_sc_scree_slope_full_deg, slope_deg)
		* (1.0 - sc_range(u_sc_scree_slope_fade_deg, u_sc_scree_slope_end_deg, slope_deg));
	scree *= clamp(0.35 + sediment * 0.9 + geology.r * 0.35, 0.0, 1.0);

	float gravel = hydro.b * (1.0 - sc_range(u_sc_gravel_slope_start_deg,
		u_sc_gravel_slope_end_deg, slope_deg));
	gravel *= clamp(0.35 + sediment * 0.9, 0.0, 1.0);
	gravel = max(gravel, hydro.a * sediment * 0.55);
	return clamp(vec4(mud, snow, scree, gravel) * u_classifier_secondary_scale,
		vec4(0.0), vec4(1.0));
}

vec3 sc_grade_albedo(vec3 c) {
	float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
	c = mix(vec3(luma), c, u_classifier_albedo_chroma);
	c = vec3(u_classifier_albedo_pivot)
		+ (c - vec3(u_classifier_albedo_pivot)) * u_classifier_albedo_contrast;
	return clamp(c, vec3(0.004), vec3(0.92));
}

vec3 sc_compose_albedo(vec4 primary, vec4 secondary, vec4 soil, vec4 surface,
		vec4 climate, float biome, float rock_id) {
	vec3 rock = sc_rock_color(rock_id);
	vec3 soil_c = sc_soil_color(soil, surface);
	vec3 vegetation = sc_vegetation_color(surface, climate, biome);
	vec3 sand = mix(u_sc_sand_low, u_sc_sand_high, soil.r);
	vec3 mud = soil_c * u_sc_mud_tint;
	vec3 snow = u_sc_snow_color;
	vec3 scree = rock * u_sc_scree_rock_mix + vec3(u_sc_scree_add);
	vec3 gravel = mix(rock, u_sc_gravel_color, u_sc_gravel_rock_mix);

	float total = dot(primary, vec4(1.0)) + dot(secondary, vec4(1.0));
	if (total < 1e-4) return sc_grade_albedo(soil_c);
	vec3 c = rock * primary.r + soil_c * primary.g + vegetation * primary.b + sand * primary.a;
	c += mud * secondary.r + snow * secondary.g + scree * secondary.b + gravel * secondary.a;
	return sc_grade_albedo(c / total);
}

float sc_compose_roughness(vec4 primary, vec4 secondary, vec4 surface) {
	float total = dot(primary, vec4(1.0)) + dot(secondary, vec4(1.0));
	float low = min(u_classifier_roughness_min, u_classifier_roughness_max);
	float high = max(u_classifier_roughness_min, u_classifier_roughness_max);
	if (total < 1e-4) return clamp(u_sc_roughness_fallback * u_classifier_roughness_scale
		+ u_classifier_roughness_bias, low, high);
	float r = u_sc_roughness_rock * primary.r
		+ u_sc_roughness_soil * primary.g
		+ u_sc_roughness_vegetation * primary.b
		+ u_sc_roughness_sand * primary.a;
	r += mix(u_sc_roughness_mud_dry, u_sc_roughness_mud_wet, surface.g) * secondary.r;
	r += u_sc_roughness_snow * secondary.g
		+ u_sc_roughness_scree * secondary.b
		+ u_sc_roughness_gravel * secondary.a;
	float result = (r / total) * u_classifier_roughness_scale + u_classifier_roughness_bias;
	return clamp(result, low, high);
}
