// GPU geomorph synthesis primitives.
// This file is self-contained apart from gpu_planet_context.gdshaderinc and
// planet_surface_lookup.gdshaderinc being included by the caller.

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
	for (int i = 0; i < 5; i++) {
		h += gm_value(p, seed + uint(i * 17)) * a;
		p = p * 2.01 + vec3(17.0, -11.0, 7.0);
		a *= 0.48;
	}
	return h;
}

float gm_ridged(vec3 p, uint seed) {
	float n = gm_fbm(p, seed);
	return 1.0 - abs(n);
}

vec3 gm_domain_warp(vec3 p, uint seed, float strength) {
	vec3 w = vec3(
		gm_fbm(p + vec3(13.1, 7.7, -4.3), seed + 101u),
		gm_fbm(p + vec3(-5.7, 19.3, 8.9), seed + 211u),
		gm_fbm(p + vec3(9.2, -3.8, 23.4), seed + 307u));
	return p + w * strength;
}

// Eight-cell approximation to 3-D Worley. Full 27-neighbour Worley is too
// expensive for every terrain vertex; this still produces broad cellular divides
// and is intended to be combined with ridged fBm and domain warping.
float gm_cellular_ridge(vec3 p, uint seed) {
	ivec3 base = ivec3(floor(p));
	float f1 = 1e9;
	float f2 = 1e9;
	for (int z = 0; z <= 1; z++) {
		for (int y = 0; y <= 1; y++) {
			for (int x = 0; x <= 1; x++) {
				ivec3 c = base + ivec3(x, y, z);
				vec3 jitter = vec3(
					gm_rand(c, seed + 3u),
					gm_rand(c, seed + 5u),
					gm_rand(c, seed + 7u));
				float d = length(p - (vec3(c) + jitter));
				if (d < f1) { f2 = f1; f1 = d; }
				else if (d < f2) { f2 = d; }
			}
		}
	}
	return clamp(1.0 - (f2 - f1) * 2.4, 0.0, 1.0);
}

vec4 gm_landform_weights(vec3 dir, float macro_h) {
	vec4 soil = asterra_ctx_soil(dir);
	vec4 surface = asterra_ctx_surface(dir);
	vec4 geology = asterra_ctx_geology(dir);
	vec4 structure = asterra_ctx_structure(dir);
	vec4 climate = asterra_ctx_climate(dir);
	vec4 hydro = asterra_ctx_hydrology(dir);

	float uplift = max(asterra_ctx_uplift_signed(structure), 0.0);
	float fault = structure.g;
	float floodplain = structure.b;
	float wetland = structure.a;
	float moisture = surface.g;
	float vegetation = surface.b;
	float sand = soil.r;
	float erodible = geology.g;
	float temp_c = asterra_ctx_temperature_c(climate);
	float precip = asterra_ctx_precip_mm(climate);

	float altitude = smoothstep(350.0, 2200.0, max(macro_h, 0.0));
	float mountain = clamp(max(uplift * 0.9 + fault * 0.55, altitude * 0.55), 0.0, 1.0);
	float arid = clamp((1.0 - moisture) * (1.0 - clamp(precip / 900.0, 0.0, 1.0))
		* (0.35 + sand * 0.85) * (1.0 - vegetation * 0.55), 0.0, 1.0);
	float cold = 1.0 - smoothstep(-12.0, 4.0, temp_c);
	float glacial = clamp(cold * (0.35 + altitude * 0.75), 0.0, 1.0);
	float depositional = clamp(max(hydro.a, floodplain * 0.85 + wetland * 0.9)
		* (0.5 + erodible * 0.35), 0.0, 1.0);
	return vec4(mountain, arid, glacial, depositional);
}

float gm_band_weight(float wavelength_m, float spacing_m) {
	float x = clamp((wavelength_m - spacing_m * 4.0) / max(spacing_m * 4.0, 1e-5), 0.0, 1.0);
	return x * x * (3.0 - 2.0 * x);
}

float gm_geomorph_height(vec3 dir, float planet_radius, float spacing_m,
		float macro_h, uint seed) {
	vec3 world_m = dir * planet_radius;
	vec4 weights = gm_landform_weights(dir, macro_h);
	vec4 soil = asterra_ctx_soil(dir);
	vec4 surface = asterra_ctx_surface(dir);
	vec4 geology = asterra_ctx_geology(dir);
	vec4 hydro = asterra_ctx_hydrology(dir);

	float mountain = weights.r;
	float arid = weights.g;
	float glacial = weights.b;
	float depositional = weights.a;
	float hardness = 1.15 - geology.g * 0.45;

	float h = 0.0;

	float w16k = gm_band_weight(16000.0, spacing_m);
	if (w16k > 0.001) {
		vec3 p = gm_domain_warp(world_m / 16000.0, seed + 11u, 0.8);
		float broad = gm_fbm(p, seed + 13u);
		h += broad * mix(24.0, 125.0, mountain) * w16k;
	}

	float w6k = gm_band_weight(6000.0, spacing_m);
	if (w6k > 0.001 && mountain > 0.02) {
		vec3 p = gm_domain_warp(world_m / 6000.0, seed + 31u, 1.1);
		float cells = gm_cellular_ridge(p, seed + 37u);
		float ridge = gm_ridged(p * 1.55, seed + 41u);
		float skeleton = mix(ridge, cells, 0.58);
		h += (skeleton * 2.0 - 1.0) * 210.0 * mountain * hardness * w6k;
	}

	float w1400 = gm_band_weight(1400.0, spacing_m);
	if (w1400 > 0.001) {
		vec3 p = gm_domain_warp(world_m / 1400.0, seed + 53u, 0.72);
		float ridge = gm_ridged(p * 1.25, seed + 59u);
		float detail = gm_fbm(p * 2.1, seed + 61u);
		h += ((ridge * 2.0 - 1.0) * 72.0 * mountain + detail * 24.0) * w1400;
	}

	// Analytic drainage incision: use the baked discharge as the large-scale
	// hydrologic prior and a warped ridged field for sub-grid tributaries.
	float w420 = gm_band_weight(420.0, spacing_m);
	if (w420 > 0.001) {
		vec3 p = gm_domain_warp(world_m / 420.0, seed + 71u, 0.55);
		float channel = pow(gm_ridged(p, seed + 73u), 4.6);
		float discharge = hydro.b;
		float incision = channel * mix(2.0, 34.0, discharge) * (1.0 - depositional * 0.78);
		h -= incision * w420;
		// Material removed from narrow channels is represented as a broader,
		// lower-amplitude depositional shoulder rather than disappearing.
		float fan = pow(gm_ridged(p * 0.48 + vec3(9.0), seed + 79u), 2.2);
		h += fan * depositional * mix(1.0, 12.0, surface.a) * w420;
	}

	float w120 = gm_band_weight(120.0, spacing_m);
	if (w120 > 0.001) {
		float n = gm_fbm(world_m / 120.0, seed + 89u);
		float dune = gm_ridged(gm_domain_warp(world_m / 180.0, seed + 97u, 0.45), seed + 101u);
		h += n * 4.5 * (1.0 - glacial * 0.6) * w120;
		h += (dune * 2.0 - 1.0) * 9.0 * arid * soil.r * w120;
	}

	float w24 = gm_band_weight(24.0, spacing_m);
	if (w24 > 0.001) {
		h += gm_value(world_m / 24.0, seed + 109u) * 0.9 * w24;
	}

	// Glacial terrain suppresses short-frequency rock noise and restores broad flow.
	if (glacial > 0.01) {
		float ice = gm_fbm(world_m / 2600.0 + vec3(2.0, -7.0, 5.0), seed + 127u);
		h = mix(h, h * 0.62 + ice * 52.0, glacial * 0.72);
	}

	return h;
}
