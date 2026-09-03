// GPU geomorph synthesis primitives.
// The caller includes gpu_planet_context.gdshaderinc first.
//
// Phase 33 exposes both production band gains and their physical geometry. Every
// new control defaults to the exact historical constant so untouched terrain is
// render-identical while Planet Studio can change wavelengths/amplitudes directly.

uniform float u_geomorph_warp_strength : hint_range(0.0, 3.0) = 1.0;
uniform float u_geomorph_biome_terrain_variation : hint_range(0.0, 1.0) = 1.0;
uniform float u_geomorph_broad_strength : hint_range(0.0, 3.0) = 1.0;
uniform float u_geomorph_mountain_strength : hint_range(0.0, 3.0) = 1.0;
uniform float u_geomorph_mid_strength : hint_range(0.0, 3.0) = 1.0;
uniform float u_geomorph_channel_strength : hint_range(0.0, 3.0) = 1.0;
uniform float u_geomorph_deposit_strength : hint_range(0.0, 3.0) = 1.0;
uniform float u_geomorph_fine_strength : hint_range(0.0, 3.0) = 1.0;
uniform float u_geomorph_dune_strength : hint_range(0.0, 3.0) = 1.0;
uniform float u_geomorph_glacial_strength : hint_range(0.0, 3.0) = 1.0;

uniform float u_geomorph_broad_wavelength_m : hint_range(1.0, 100000.0) = 16000.0;
uniform float u_geomorph_broad_low_amplitude_m : hint_range(0.0, 2000.0) = 24.0;
uniform float u_geomorph_broad_mountain_amplitude_m : hint_range(0.0, 4000.0) = 125.0;
uniform float u_geomorph_broad_warp : hint_range(0.0, 4.0) = 0.8;
uniform float u_geomorph_mountain_wavelength_m : hint_range(1.0, 100000.0) = 6000.0;
uniform float u_geomorph_mountain_amplitude_m : hint_range(0.0, 4000.0) = 210.0;
uniform float u_geomorph_mountain_warp : hint_range(0.0, 4.0) = 1.1;
uniform float u_geomorph_mountain_ridge_scale : hint_range(0.05, 8.0) = 1.55;
uniform float u_geomorph_mountain_cell_mix : hint_range(0.0, 1.0) = 0.58;
uniform float u_geomorph_mid_wavelength_m : hint_range(1.0, 50000.0) = 1400.0;
uniform float u_geomorph_mid_ridge_amplitude_m : hint_range(0.0, 2000.0) = 72.0;
uniform float u_geomorph_mid_noise_amplitude_m : hint_range(0.0, 1000.0) = 24.0;
uniform float u_geomorph_mid_warp : hint_range(0.0, 4.0) = 0.72;
uniform float u_geomorph_mid_ridge_scale : hint_range(0.05, 8.0) = 1.25;
uniform float u_geomorph_mid_detail_scale : hint_range(0.05, 8.0) = 2.1;
uniform float u_geomorph_channel_wavelength_m : hint_range(1.0, 10000.0) = 420.0;
uniform float u_geomorph_channel_depth_min_m : hint_range(0.0, 1000.0) = 2.0;
uniform float u_geomorph_channel_depth_max_m : hint_range(0.0, 2000.0) = 34.0;
uniform float u_geomorph_channel_warp : hint_range(0.0, 4.0) = 0.55;
uniform float u_geomorph_channel_power : hint_range(0.1, 12.0) = 4.6;
uniform float u_geomorph_flow_along_scale : hint_range(0.01, 4.0) = 0.42;
uniform float u_geomorph_flow_across_scale : hint_range(0.01, 4.0) = 1.45;
uniform float u_geomorph_deposit_amplitude_min_m : hint_range(0.0, 500.0) = 1.0;
uniform float u_geomorph_deposit_amplitude_max_m : hint_range(0.0, 1000.0) = 12.0;
uniform float u_geomorph_deposit_scale : hint_range(0.01, 4.0) = 0.48;
uniform float u_geomorph_deposit_power : hint_range(0.1, 8.0) = 2.2;
uniform float u_geomorph_fine_wavelength_m : hint_range(0.5, 5000.0) = 120.0;
uniform float u_geomorph_fine_amplitude_m : hint_range(0.0, 250.0) = 4.5;
uniform float u_geomorph_dune_wavelength_m : hint_range(0.5, 5000.0) = 180.0;
uniform float u_geomorph_dune_amplitude_m : hint_range(0.0, 500.0) = 9.0;
uniform float u_geomorph_dune_warp : hint_range(0.0, 4.0) = 0.45;
uniform float u_geomorph_micro_wavelength_m : hint_range(0.25, 1000.0) = 24.0;
uniform float u_geomorph_micro_amplitude_m : hint_range(0.0, 100.0) = 0.9;
uniform float u_geomorph_glacial_wavelength_m : hint_range(1.0, 50000.0) = 2600.0;
uniform float u_geomorph_glacial_amplitude_m : hint_range(0.0, 2000.0) = 52.0;
uniform float u_geomorph_glacial_base_scale : hint_range(0.0, 2.0) = 0.62;
uniform float u_geomorph_glacial_mix : hint_range(0.0, 2.0) = 0.72;

uint gm_hash(ivec3 p, uint seed) {
	uint h = uint(p.x) * 0x8da6b343u;
	h ^= uint(p.y) * 0xd8163841u;
	h ^= uint(p.z) * 0xcb1ab31fu;
	h ^= seed * 0x9e3779b9u;
	h ^= h >> uint(16);
	h *= 0x7feb352du;
	h ^= h >> uint(15);
	h *= 0x846ca68bu;
	h ^= h >> uint(16);
	return h;
}

float gm_rand(ivec3 p, uint seed) {
	return float(gm_hash(p, seed) & 0x00ffffffu) / 16777215.0;
}

vec3 gm_rand3(ivec3 p, uint seed) {
	uint h0 = gm_hash(p, seed);
	uint h1 = (h0 ^ (h0 >> uint(13)) ^ 0x68bc21ebu) * 0x9e3779b9u;
	uint h2 = (h0 ^ (h0 << uint(11)) ^ 0x02e5be93u) * 0x85ebca6bu;
	h1 ^= h1 >> uint(16);
	h2 ^= h2 >> uint(15);
	return vec3(
		float(h0 & 0x00ffffffu),
		float(h1 & 0x00ffffffu),
		float(h2 & 0x00ffffffu)) / 16777215.0;
}

float gm_value(vec3 p, uint seed) {
	ivec3 i = ivec3(floor(p));
	vec3 f = fract(p);
	f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
	float a = mix(gm_rand(i + ivec3(0,0,0), seed), gm_rand(i + ivec3(1,0,0), seed), f.x);
	float b = mix(gm_rand(i + ivec3(0,1,0), seed), gm_rand(i + ivec3(1,1,0), seed), f.x);
	float c = mix(gm_rand(i + ivec3(0,0,1), seed), gm_rand(i + ivec3(1,0,1), seed), f.x);
	float d = mix(gm_rand(i + ivec3(0,1,1), seed), gm_rand(i + ivec3(1,1,1), seed), f.x);
	return mix(mix(a, b, f.y), mix(c, d, f.y), f.z) * 2.0 - 1.0;
}

float gm_fbm(vec3 p, uint seed) {
	float h = 0.0;
	float a = 0.52;
	for (int i = 0; i < 3; i++) {
		h += gm_value(p, seed + uint(i * 17)) * a;
		p = p * 2.01 + vec3(17.0, -11.0, 7.0);
		a *= 0.48;
	}
	return h * 1.0957;
}

float gm_fbm_warp(vec3 p, uint seed) {
	float h = 0.0;
	float a = 0.52;
	for (int i = 0; i < 2; i++) {
		h += gm_value(p, seed + uint(i * 17)) * a;
		p = p * 2.01 + vec3(17.0, -11.0, 7.0);
		a *= 0.48;
	}
	return h * 1.2662;
}

float gm_ridged(vec3 p, uint seed) {
	return 1.0 - abs(gm_fbm(p, seed));
}

vec3 gm_domain_warp(vec3 p, uint seed, float strength) {
	vec3 w = vec3(
		gm_fbm_warp(p + vec3(13.1, 7.7, -4.3), seed + 101u),
		gm_fbm_warp(p + vec3(-5.7, 19.3, 8.9), seed + 211u),
		gm_fbm_warp(p + vec3(9.2, -3.8, 23.4), seed + 307u));
	return p + w * strength * u_geomorph_warp_strength;
}

float gm_cellular_ridge(vec3 p, uint seed) {
	ivec3 base = ivec3(floor(p));
	float f1 = 1e9;
	float f2 = 1e9;
	for (int z = -1; z <= 1; z++) {
		for (int y = -1; y <= 1; y++) {
			for (int x = -1; x <= 1; x++) {
				ivec3 c = base + ivec3(x, y, z);
				vec3 jitter = gm_rand3(c, seed + 3u);
				vec3 feature = vec3(c) + vec3(0.25) + jitter * 0.50;
				vec3 delta = p - feature;
				float d2 = dot(delta, delta);
				if (d2 < f1) {
					f2 = f1;
					f1 = d2;
				} else if (d2 < f2) {
					f2 = d2;
				}
			}
		}
	}
	return clamp(1.0 - (f2 - f1) * 2.8, 0.0, 1.0);
}

vec4 gm_landform_weights_from_context(float macro_h, vec4 soil, vec4 surface,
		vec4 geology, vec4 structure, vec4 climate, vec4 hydro) {
	float uplift = max(asterra_ctx_uplift_signed(structure), 0.0);
	float fault = structure.g;
	float plate_boundary = geology.a;
	float floodplain = structure.b;
	float wetland = structure.a;
	float moisture = surface.g;
	float vegetation = surface.b;
	float sand = soil.r;
	float erodible = geology.r;
	float temp_c = asterra_ctx_temperature_c(climate);
	float precip = asterra_ctx_precip_mm(climate);

	float altitude = smoothstep(450.0, 2600.0, max(macro_h, 0.0));
	float tectonic = clamp(uplift * 0.82 + fault * 0.50 + plate_boundary * 0.28, 0.0, 1.0);
	float mountain = clamp(max(tectonic, altitude * 0.50), 0.0, 1.0);
	float arid = clamp((1.0 - moisture) * (1.0 - clamp(precip / 900.0, 0.0, 1.0))
		* (0.35 + sand * 0.85) * (1.0 - vegetation * 0.55), 0.0, 1.0);
	float cold = 1.0 - smoothstep(-12.0, 4.0, temp_c);
	float glacial = clamp(cold * (0.35 + altitude * 0.75), 0.0, 1.0);
	float depositional = clamp(max(hydro.a, floodplain * 0.85 + wetland * 0.9)
		* (0.5 + erodible * 0.35), 0.0, 1.0);
	return vec4(mountain, arid, glacial, depositional);
}

vec4 gm_landform_weights(vec3 dir, float macro_h) {
	return gm_landform_weights_from_context(macro_h,
		asterra_ctx_soil(dir), asterra_ctx_surface(dir), asterra_ctx_geology(dir),
		asterra_ctx_structure(dir), asterra_ctx_climate(dir), asterra_ctx_hydrology(dir));
}

float gm_band_weight(float wavelength_m, float spacing_m) {
	float four_spacing = spacing_m * 4.0;
	float x = clamp((wavelength_m - four_spacing) / max(four_spacing, 1e-5), 0.0, 1.0);
	return x * x * (3.0 - 2.0 * x);
}

vec3 gm_flow_world(vec3 dir, vec2 flow_xy) {
	float flow_len_sq = dot(flow_xy, flow_xy);
	if (flow_len_sq <= 1e-10) return vec3(0.0);
	vec3 basis_up = abs(dir.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	vec3 east = normalize(cross(basis_up, dir));
	vec3 north = normalize(cross(dir, east));
	vec3 flow = east * flow_xy.x + north * flow_xy.y;
	return flow * inversesqrt(max(dot(flow, flow), 1e-10));
}

float gm_geomorph_height(vec3 dir, float planet_radius, float spacing_m,
		float macro_h, uint seed) {
	vec3 world_m = dir * planet_radius;
	vec4 soil = asterra_ctx_soil(dir);
	vec4 surface = asterra_ctx_surface(dir);
	vec4 geology = asterra_ctx_geology(dir);
	vec4 structure = asterra_ctx_structure(dir);
	vec4 climate = asterra_ctx_climate(dir);
	vec4 hydro = asterra_ctx_hydrology(dir);
	vec4 weights = gm_landform_weights_from_context(
		macro_h, soil, surface, geology, structure, climate, hydro);
	// Height-synthesis only: fade the per-biome landform weighting toward neutral
	// so Global Terrain can be made uniform everywhere. Material classification
	// calls gm_landform_weights_from_context directly and is not affected.
	weights = mix(vec4(0.0), weights, u_geomorph_biome_terrain_variation);

	float mountain = weights.r;
	float arid = weights.g;
	float glacial = weights.b;
	float depositional = weights.a;
	float hardness = 1.20 - geology.r * 0.48;
	float h = 0.0;

	float broad_wavelength = max(u_geomorph_broad_wavelength_m, 0.001);
	float w16k = gm_band_weight(broad_wavelength, spacing_m);
	if (w16k > 0.001) {
		vec3 p = gm_domain_warp(world_m / broad_wavelength, seed + 11u, u_geomorph_broad_warp);
		float broad = gm_fbm(p, seed + 13u);
		h += broad * mix(u_geomorph_broad_low_amplitude_m,
			u_geomorph_broad_mountain_amplitude_m, mountain) * w16k * u_geomorph_broad_strength;
	}

	float mountain_wavelength = max(u_geomorph_mountain_wavelength_m, 0.001);
	float w6k = gm_band_weight(mountain_wavelength, spacing_m);
	float mountain_gate = smoothstep(0.04, 0.08, mountain);
	if (w6k > 0.001 && mountain > 0.04) {
		vec3 p = gm_domain_warp(world_m / mountain_wavelength, seed + 31u, u_geomorph_mountain_warp);
		float cells = gm_cellular_ridge(p, seed + 37u);
		float ridge = gm_ridged(p * max(u_geomorph_mountain_ridge_scale, 0.001), seed + 41u);
		float skeleton = mix(ridge, cells, clamp(u_geomorph_mountain_cell_mix, 0.0, 1.0));
		h += (skeleton * 2.0 - 1.0) * u_geomorph_mountain_amplitude_m * mountain * mountain_gate
			* hardness * w6k * u_geomorph_mountain_strength;
	}

	float mid_wavelength = max(u_geomorph_mid_wavelength_m, 0.001);
	float w1400 = gm_band_weight(mid_wavelength, spacing_m);
	if (w1400 > 0.001) {
		vec3 p = gm_domain_warp(world_m / mid_wavelength, seed + 53u, u_geomorph_mid_warp);
		float ridge = gm_ridged(p * max(u_geomorph_mid_ridge_scale, 0.001), seed + 59u);
		float detail = gm_fbm(p * max(u_geomorph_mid_detail_scale, 0.001), seed + 61u);
		h += ((ridge * 2.0 - 1.0) * u_geomorph_mid_ridge_amplitude_m * mountain
			+ detail * u_geomorph_mid_noise_amplitude_m) * w1400 * u_geomorph_mid_strength;
	}

	float channel_wavelength = max(u_geomorph_channel_wavelength_m, 0.001);
	float w420 = gm_band_weight(channel_wavelength, spacing_m);
	if (w420 > 0.001) {
		vec2 raw_flow = asterra_ctx_flow(hydro);
		float flow_mag = length(raw_flow);
		float flow_gate = smoothstep(0.02, 0.20, flow_mag);
		vec3 sample_m = world_m;
		if (flow_mag > 1e-5) {
			vec3 flow = gm_flow_world(dir, raw_flow);
			float along_q = dot(world_m, flow);
			vec3 along = flow * along_q;
			vec3 across = world_m - along;
			vec3 warped_m = along * u_geomorph_flow_along_scale + across * u_geomorph_flow_across_scale;
			sample_m = mix(world_m, warped_m, flow_gate);
		}
		vec3 p = gm_domain_warp(sample_m / channel_wavelength, seed + 71u, u_geomorph_channel_warp);
		float channel = pow(gm_ridged(p, seed + 73u), max(u_geomorph_channel_power, 0.01));
		float incision = channel * mix(u_geomorph_channel_depth_min_m,
			u_geomorph_channel_depth_max_m, hydro.b) * (1.0 - depositional * 0.78);
		h -= incision * w420 * u_geomorph_channel_strength;
		float depositional_gate = smoothstep(0.0, 0.03, depositional);
		if (depositional > 0.0) {
			float fan = pow(gm_ridged(p * u_geomorph_deposit_scale + vec3(9.0), seed + 79u),
				max(u_geomorph_deposit_power, 0.01));
			h += fan * depositional * depositional_gate
				* mix(u_geomorph_deposit_amplitude_min_m, u_geomorph_deposit_amplitude_max_m, surface.a)
				* w420 * u_geomorph_deposit_strength;
		}
	}

	float fine_wavelength = max(u_geomorph_fine_wavelength_m, 0.001);
	float w120 = gm_band_weight(fine_wavelength, spacing_m);
	if (w120 > 0.001) {
		float n = gm_fbm(world_m / fine_wavelength, seed + 89u);
		h += n * u_geomorph_fine_amplitude_m * (1.0 - glacial * 0.6)
			* w120 * u_geomorph_fine_strength;
		float dune_weight = arid * soil.r;
		float dune_gate = smoothstep(0.0, 0.03, dune_weight);
		if (dune_weight > 0.0) {
			float dune_wavelength = max(u_geomorph_dune_wavelength_m, 0.001);
			float dune = gm_ridged(gm_domain_warp(world_m / dune_wavelength,
				seed + 97u, u_geomorph_dune_warp), seed + 101u);
			h += (dune * 2.0 - 1.0) * u_geomorph_dune_amplitude_m * dune_weight
				* dune_gate * w120 * u_geomorph_dune_strength;
		}
	}

	float micro_wavelength = max(u_geomorph_micro_wavelength_m, 0.001);
	float w24 = gm_band_weight(micro_wavelength, spacing_m);
	if (w24 > 0.001) {
		h += gm_value(world_m / micro_wavelength, seed + 109u)
			* u_geomorph_micro_amplitude_m * w24 * u_geomorph_fine_strength;
	}

	float glacial_gate = smoothstep(0.0, 0.05, glacial);
	if (glacial > 0.0) {
		float glacial_wavelength = max(u_geomorph_glacial_wavelength_m, 0.001);
		float ice = gm_fbm(world_m / glacial_wavelength + vec3(2.0, -7.0, 5.0), seed + 127u);
		float glacial_mix = clamp(glacial * glacial_gate * u_geomorph_glacial_mix
			* u_geomorph_glacial_strength, 0.0, 1.0);
		h = mix(h, h * u_geomorph_glacial_base_scale
			+ ice * u_geomorph_glacial_amplitude_m * u_geomorph_glacial_strength, glacial_mix);
	}

	return h;
}
