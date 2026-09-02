// Geology-specific procedural PBR for exposed bedrock.
//
// Rock family is categorical coarse world state (0..12). This layer keeps that
// authoritative family identity while adding material response that the generic
// surface classifier cannot express: family roughness/specular, crystalline vs
// sedimentary colour structure, foliation/bedding, fractures and close normal
// detail. No extra textures are required, so the scan sample budget is unchanged.
//
// Phase 33 exposes the stage's physical detail scale, distance response,
// weatherability and fracture response. Cell sizes are supplied with exact periods
// by the runtime so the 4096 m floating-origin wrap remains phase continuous.

uniform float u_rock_pbr_enabled = 1.0;
uniform float u_rock_detail_strength : hint_range(0.0, 2.0) = 1.0;
uniform float u_rock_normal_strength : hint_range(0.0, 2.0) = 1.0;
uniform float u_rock_color_strength : hint_range(0.0, 2.0) = 1.0;
uniform float u_rock_coarse_cell_m : hint_range(1.0, 512.0) = 4.0;
uniform float u_rock_fine_cell_m : hint_range(1.0, 512.0) = 1.0;
uniform int u_rock_coarse_period = 1024;
uniform int u_rock_fine_period = 4096;
uniform int u_rock_coarse_seed = 11665;
uniform int u_rock_fine_seed = 35415;
uniform float u_rock_coarse_fade_near_m : hint_range(0.0, 20000.0) = 850.0;
uniform float u_rock_coarse_fade_far_m : hint_range(0.0, 50000.0) = 3400.0;
uniform float u_rock_fine_fade_near_m : hint_range(0.0, 10000.0) = 130.0;
uniform float u_rock_fine_fade_far_m : hint_range(0.0, 20000.0) = 920.0;
uniform float u_rock_normal_fade_near_m : hint_range(0.0, 10000.0) = 90.0;
uniform float u_rock_normal_fade_far_m : hint_range(0.0, 20000.0) = 620.0;
uniform float u_rock_weatherability_pivot : hint_range(0.0, 1.0) = 0.45;
uniform float u_rock_weatherability_roughness : hint_range(-0.5, 0.5) = 0.055;
uniform float u_rock_family_roughness_min : hint_range(0.0, 1.0) = 0.48;
uniform float u_rock_family_roughness_max : hint_range(0.0, 1.0) = 0.97;
uniform float u_rock_fracture_color_base : hint_range(0.0, 1.0) = 0.09;
uniform float u_rock_fracture_color_fault : hint_range(0.0, 1.0) = 0.15;
uniform float u_rock_fracture_roughness : hint_range(0.0, 0.5) = 0.040;
uniform float u_rock_normal_fracture_mix : hint_range(0.0, 2.0) = 0.55;
uniform float u_rock_output_roughness_min : hint_range(0.0, 1.0) = 0.45;
uniform float u_rock_output_roughness_max : hint_range(0.0, 1.0) = 0.98;

void rrp_family_traits(float rock_id, out float family_roughness,
		out float family_specular, out float normal_gain, out float colour_gain) {
	// Values are dry, weathered surface targets. They intentionally stay within
	// the existing terrain shader's conservative dielectric response range.
	if (rock_id < 0.5) { // granite
		family_roughness = 0.70; family_specular = 0.24; normal_gain = 0.20; colour_gain = 0.18;
	} else if (rock_id < 1.5) { // basalt
		family_roughness = 0.82; family_specular = 0.18; normal_gain = 0.16; colour_gain = 0.09;
	} else if (rock_id < 2.5) { // gabbro
		family_roughness = 0.70; family_specular = 0.22; normal_gain = 0.23; colour_gain = 0.17;
	} else if (rock_id < 3.5) { // gneiss
		family_roughness = 0.72; family_specular = 0.24; normal_gain = 0.29; colour_gain = 0.23;
	} else if (rock_id < 4.5) { // schist
		family_roughness = 0.64; family_specular = 0.30; normal_gain = 0.34; colour_gain = 0.19;
	} else if (rock_id < 5.5) { // sandstone
		family_roughness = 0.91; family_specular = 0.14; normal_gain = 0.24; colour_gain = 0.21;
	} else if (rock_id < 6.5) { // shale
		family_roughness = 0.86; family_specular = 0.18; normal_gain = 0.28; colour_gain = 0.17;
	} else if (rock_id < 7.5) { // limestone
		family_roughness = 0.82; family_specular = 0.18; normal_gain = 0.20; colour_gain = 0.15;
	} else if (rock_id < 8.5) { // dolomite
		family_roughness = 0.84; family_specular = 0.17; normal_gain = 0.20; colour_gain = 0.14;
	} else if (rock_id < 9.5) { // conglomerate
		family_roughness = 0.88; family_specular = 0.16; normal_gain = 0.31; colour_gain = 0.25;
	} else if (rock_id < 10.5) { // quartzite
		family_roughness = 0.58; family_specular = 0.32; normal_gain = 0.20; colour_gain = 0.11;
	} else if (rock_id < 11.5) { // rhyolitic tuff
		family_roughness = 0.94; family_specular = 0.12; normal_gain = 0.34; colour_gain = 0.23;
	} else { // serpentinite
		family_roughness = 0.61; family_specular = 0.30; normal_gain = 0.29; colour_gain = 0.21;
	}
}

vec3 rrp_tangent_component(vec3 v, vec3 n) {
	return v - n * dot(v, n);
}

void rrp_apply(vec3 detail_position, vec3 geom_n, float camera_dist,
		float rock_id, vec4 geology, vec4 structure, float rock_mix_w,
		vec3 base_albedo, float base_roughness,
		out vec3 albedo, out float roughness, out float specular,
		out vec3 rock_normal, out float rock_normal_w) {
	albedo = base_albedo;
	roughness = base_roughness;
	specular = 0.22;
	rock_normal = geom_n;
	rock_normal_w = 0.0;
	if (u_rock_pbr_enabled < 0.5 || rock_mix_w <= 0.004) return;

	float family_roughness;
	float family_specular;
	float normal_gain;
	float colour_gain;
	rrp_family_traits(rock_id, family_roughness, family_specular, normal_gain, colour_gain);

	float weatherability = clamp(geology.r, 0.0, 1.0);
	float strata = clamp(geology.g, 0.0, 1.0);
	float basin = clamp(geology.b, 0.0, 1.0);
	float fault = clamp(structure.g, 0.0, 1.0);
	float family_min = min(u_rock_family_roughness_min, u_rock_family_roughness_max);
	float family_max = max(u_rock_family_roughness_min, u_rock_family_roughness_max);
	family_roughness = clamp(family_roughness
		+ (weatherability - u_rock_weatherability_pivot) * u_rock_weatherability_roughness,
		family_min, family_max);
	roughness = mix(roughness, family_roughness, rock_mix_w);
	specular = mix(specular, family_specular, rock_mix_w);

	float coarse_near = min(u_rock_coarse_fade_near_m, u_rock_coarse_fade_far_m);
	float coarse_far = max(u_rock_coarse_fade_near_m, u_rock_coarse_fade_far_m);
	float fine_near = min(u_rock_fine_fade_near_m, u_rock_fine_fade_far_m);
	float fine_far = max(u_rock_fine_fade_near_m, u_rock_fine_fade_far_m);
	float normal_near = min(u_rock_normal_fade_near_m, u_rock_normal_fade_far_m);
	float normal_far = max(u_rock_normal_fade_near_m, u_rock_normal_fade_far_m);
	float coarse_w = 1.0 - smoothstep(coarse_near, max(coarse_far, coarse_near + 0.001), camera_dist);
	if (coarse_w <= 0.002) return;
	float fine_w = 1.0 - smoothstep(fine_near, max(fine_far, fine_near + 0.001), camera_dist);
	float normal_distance_w = 1.0 - smoothstep(normal_near, max(normal_far, normal_near + 0.001), camera_dist);

	float coarse_cell = max(u_rock_coarse_cell_m, 0.001);
	float fine_cell = max(u_rock_fine_cell_m, 0.001);
	vec3 coarse = spat_periodic_value(
		detail_position / coarse_cell, max(u_rock_coarse_period, 1), uint(max(u_rock_coarse_seed, 1)));
	vec3 fine = vec3(0.0);
	if (fine_w > 0.002) {
		fine = spat_periodic_value(
			detail_position / fine_cell, max(u_rock_fine_period, 1), uint(max(u_rock_fine_seed, 1)));
	}

	float coarse_s = dot(coarse, vec3(0.54, 0.31, 0.15));
	float fine_s = dot(fine, vec3(0.28, 0.51, 0.21));
	float band = sin((coarse.x * 0.72 + coarse.y * 0.28) * 9.0 + fine.z * 1.35);
	float thin_band = sin((coarse.y * 0.58 + fine.x * 0.42) * 15.0);
	float clast = smoothstep(0.24, 0.69, coarse.x * 0.62 + fine.y * 0.38);
	float pore = smoothstep(0.38, 0.78, -(coarse.y * 0.55 + fine.z * 0.45));
	float vein = 1.0 - smoothstep(0.035, 0.16,
		abs(coarse.z * 0.78 + fine.x * 0.22));
	float fracture = 1.0 - smoothstep(0.025, 0.13,
		abs(coarse.x * 0.61 - coarse.z * 0.39 + fine.y * 0.16));
	fracture *= 0.16 + fault * 0.84;

	vec3 rock_base = sc_rock_color(rock_id);
	vec3 rock_colour = rock_base;
	float rough_delta = 0.0;
	float normal_pattern = 0.65 + abs(fine_s) * 0.55;

	if (rock_id < 0.5) {
		float felsic = smoothstep(-0.20, 0.58, coarse_s);
		float mafic = smoothstep(0.42, 0.78, -fine_s);
		rock_colour *= 0.94 + coarse_s * 0.10;
		rock_colour = mix(rock_colour, vec3(0.405, 0.365, 0.335), felsic * 0.16);
		rock_colour = mix(rock_colour, vec3(0.105, 0.105, 0.102), mafic * 0.13);
		rough_delta -= felsic * 0.025;
	} else if (rock_id < 1.5) {
		rock_colour *= 0.96 + fine_s * 0.055;
		rock_colour *= 1.0 - pore * 0.10;
		rough_delta += pore * 0.045;
		normal_pattern *= 0.72;
	} else if (rock_id < 2.5) {
		float light_crystal = smoothstep(0.18, 0.72, coarse_s);
		rock_colour *= 0.91 + fine_s * 0.10;
		rock_colour = mix(rock_colour, vec3(0.245, 0.240, 0.220), light_crystal * 0.22);
		normal_pattern *= 1.10;
	} else if (rock_id < 3.5) {
		float b = band * (0.70 + strata * 0.45);
		rock_colour *= 1.0 + b * 0.16;
		rock_colour = mix(rock_colour, vec3(0.105, 0.105, 0.100), smoothstep(0.45, 0.95, -b) * 0.23);
		rough_delta += abs(b) * 0.018;
		normal_pattern *= 1.0 + abs(b) * 0.42;
	} else if (rock_id < 4.5) {
		float b = thin_band * (0.72 + strata * 0.52);
		rock_colour *= 1.0 + b * 0.105 + fine_s * 0.045;
		rough_delta -= smoothstep(0.50, 0.95, b) * 0.055;
		normal_pattern *= 1.10 + abs(b) * 0.48;
	} else if (rock_id < 5.5) {
		float b = band * (0.56 + basin * 0.38 + strata * 0.30);
		rock_colour *= vec3(1.0 + b * 0.13, 1.0 + b * 0.055, 1.0 - b * 0.045);
		rock_colour *= 0.97 + fine_s * 0.075;
		rough_delta += abs(fine_s) * 0.025;
		normal_pattern *= 0.88 + abs(b) * 0.30;
	} else if (rock_id < 6.5) {
		float b = thin_band * (0.76 + basin * 0.34 + strata * 0.36);
		rock_colour *= 0.98 + b * 0.095 + fine_s * 0.035;
		rough_delta += smoothstep(0.35, 0.95, abs(b)) * 0.028;
		normal_pattern *= 1.05 + abs(b) * 0.52;
	} else if (rock_id < 7.5) {
		rock_colour *= 1.0 + coarse_s * 0.075;
		rock_colour = mix(rock_colour, rock_base * 0.54, pore * 0.18);
		rough_delta += pore * 0.055;
		normal_pattern *= 0.78 + pore * 0.55;
	} else if (rock_id < 8.5) {
		rock_colour *= vec3(1.0 + coarse_s * 0.070, 1.0 + coarse_s * 0.050, 1.0 + coarse_s * 0.022);
		rock_colour *= 0.985 + fine_s * 0.040;
		normal_pattern *= 0.80;
	} else if (rock_id < 9.5) {
		vec3 clast_colour = mix(vec3(0.18, 0.155, 0.125), vec3(0.39, 0.34, 0.28), coarse.z * 0.5 + 0.5);
		rock_colour = mix(rock_colour * (0.90 + fine_s * 0.06), clast_colour, clast * 0.44);
		rough_delta += clast * 0.035;
		normal_pattern *= 0.90 + clast * 0.72;
	} else if (rock_id < 10.5) {
		float crystal = smoothstep(0.28, 0.78, fine_s);
		rock_colour *= 0.985 + coarse_s * 0.045;
		rock_colour = mix(rock_colour, vec3(0.545, 0.530, 0.510), crystal * 0.10);
		rough_delta -= crystal * 0.060;
		normal_pattern *= 0.74;
	} else if (rock_id < 11.5) {
		rock_colour *= 0.96 + coarse_s * 0.12 + fine_s * 0.055;
		rock_colour = mix(rock_colour, rock_base * 0.55, pore * 0.24);
		rough_delta += pore * 0.070;
		normal_pattern *= 0.95 + pore * 0.85;
	} else {
		rock_colour *= vec3(0.98 + coarse_s * 0.03, 1.0 + coarse_s * 0.10, 0.98 + coarse_s * 0.025);
		rock_colour = mix(rock_colour, vec3(0.255, 0.295, 0.235), vein * 0.28);
		rough_delta -= vein * 0.070;
		normal_pattern *= 0.92 + vein * 0.58;
	}

	rock_colour *= 1.0 - fracture * (u_rock_fracture_color_base + fault * u_rock_fracture_color_fault);
	rough_delta += fracture * u_rock_fracture_roughness;

	float colour_w = rock_mix_w * coarse_w * u_rock_detail_strength * u_rock_color_strength;
	albedo = mix(albedo, clamp(rock_colour, vec3(0.025), vec3(0.62)), clamp(colour_w, 0.0, 1.0));
	float output_min = min(u_rock_output_roughness_min, u_rock_output_roughness_max);
	float output_max = max(u_rock_output_roughness_min, u_rock_output_roughness_max);
	roughness = clamp(roughness + rough_delta * rock_mix_w * coarse_w * u_rock_detail_strength,
		output_min, output_max);

	if (normal_distance_w > 0.002 && fine_w > 0.002) {
		vec3 tangent_fine = rrp_tangent_component(fine, geom_n);
		vec3 tangent_coarse = rrp_tangent_component(coarse, geom_n);
		vec3 perturb = tangent_fine * normal_pattern
			+ tangent_coarse * fracture * u_rock_normal_fracture_mix;
		float plen = length(perturb);
		if (plen > 1e-5) perturb /= plen;
		float strength = normal_gain * normal_distance_w * fine_w
			* u_rock_detail_strength * u_rock_normal_strength;
		rock_normal = normalize(geom_n + perturb * strength);
		rock_normal_w = clamp(rock_mix_w * normal_distance_w, 0.0, 1.0);
	}
}
