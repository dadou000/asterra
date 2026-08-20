/// Shared weather-driven cloud morphology and vertical layer model.
///
/// This file owns the macro cloud geometry used by both the visible volumetric
/// renderer and the cheap lighting/shadow paths. Keeping coverage masks,
/// cloud bases/tops and vertical profiles in one place prevents a visible cloud
/// from casting a shadow from a different altitude.
///
/// Fine 3D erosion belongs to the visible volume. The functions here are
/// inexpensive direction/altitude fields suitable for secondary light rays.

float cloud_struct_hash(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.yzx + 33.33);
	return fract((p.x + p.y) * p.z);
}

float cloud_struct_vnoise(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(
		mix(mix(cloud_struct_hash(i), cloud_struct_hash(i + vec3(1.0, 0.0, 0.0)), f.x),
			mix(cloud_struct_hash(i + vec3(0.0, 1.0, 0.0)), cloud_struct_hash(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
		mix(mix(cloud_struct_hash(i + vec3(0.0, 0.0, 1.0)), cloud_struct_hash(i + vec3(1.0, 0.0, 1.0)), f.x),
			mix(cloud_struct_hash(i + vec3(0.0, 1.0, 1.0)), cloud_struct_hash(i + vec3(1.0, 1.0, 1.0)), f.x), f.y),
		f.z);
}

/// Low/deep-convective geography. Rough feature sizes on a 1000 km radius
/// planet are tens of kilometres rather than face-sized slabs.
vec2 cloud_structure_noises(vec3 dir) {
	vec3 d = normalize(dir);
	float synoptic = cloud_struct_vnoise(d * 17.0 + vec3(7.1, -3.4, 5.7));
	float mesoscale = cloud_struct_vnoise(d * 53.0 + vec3(-13.2, 8.6, 21.4));
	return vec2(synoptic, mesoscale);
}

/// High cloud/anvil geography is intentionally decorrelated from the low layer.
vec2 cloud_high_structure_noises(vec3 dir) {
	vec3 d = normalize(dir);
	float synoptic = cloud_struct_vnoise(d * 11.0 + vec3(-19.4, 12.1, 4.8));
	float mesoscale = cloud_struct_vnoise(d * 37.0 + vec3(24.7, -6.3, 15.9));
	return vec2(synoptic, mesoscale);
}

float cloud_type_from_weather(vec4 weather) {
	return clamp(weather.g, 0.0, 1.0);
}

float cloud_base_from_weather(vec4 weather) {
	return clamp(weather.a * 6000.0, 700.0, 6500.0);
}

float cloud_low_top_from_weather(vec4 weather) {
	float base = cloud_base_from_weather(weather);
	float coverage = clamp(weather.r, 0.0, 1.0);
	float type = cloud_type_from_weather(weather);

	float stratus_top = base + mix(650.0, 1750.0, coverage);
	float cumulus_top = base + mix(1800.0, 4800.0, coverage);
	float deep_top = base + mix(6500.0, 14500.0, pow(type, 0.78));

	float cumulus_mix = smoothstep(0.16, 0.54, type);
	float deep_mix = smoothstep(0.58, 0.88, type);
	float top = mix(stratus_top, cumulus_top, cumulus_mix);
	top = mix(top, deep_top, deep_mix);
	return min(top, 17800.0);
}

float cloud_high_base_from_weather(vec4 weather) {
	float high_cov = clamp(weather.b, 0.0, 1.0);
	float type = cloud_type_from_weather(weather);
	float base = mix(11200.0, 9300.0, high_cov);
	base = mix(base, 8500.0, smoothstep(0.62, 0.92, type));
	return base;
}

float cloud_high_top_from_weather(vec4 weather) {
	float high_cov = clamp(weather.b, 0.0, 1.0);
	float type = cloud_type_from_weather(weather);
	float top = mix(14500.0, 17400.0, max(high_cov, type * 0.78));
	return min(max(top, cloud_high_base_from_weather(weather) + 900.0), 17900.0);
}

float cloud_low_structure_field(vec4 weather, vec2 noises) {
	float coverage = clamp(weather.r, 0.0, 1.0);
	float convective = clamp(weather.g, 0.0, 1.0);
	float edge_freedom = 1.0 - smoothstep(0.78, 0.97, coverage);
	float perturb = (noises.x - 0.5) * 0.31 + (noises.y - 0.5) * 0.13;
	return coverage + perturb * edge_freedom + convective * 0.045;
}

float cloud_high_structure_field(vec4 weather, vec2 noises) {
	float high_cov = clamp(weather.b, 0.0, 1.0);
	float convective = clamp(weather.g, 0.0, 1.0);
	float edge_freedom = 1.0 - smoothstep(0.72, 0.95, high_cov);
	float perturb = (noises.x - 0.5) * 0.28 + (noises.y - 0.5) * 0.15;
	return high_cov + perturb * edge_freedom + convective * 0.06;
}

float cloud_low_structure_mask(vec4 weather, vec2 noises) {
	return smoothstep(0.18, 0.51, cloud_low_structure_field(weather, noises));
}

float cloud_high_structure_mask(vec4 weather, vec2 noises) {
	return smoothstep(0.12, 0.46, cloud_high_structure_field(weather, noises));
}

float cloud_low_edge_interior(vec4 weather, vec2 noises) {
	return smoothstep(0.51, 0.82, cloud_low_structure_field(weather, noises));
}

float cloud_high_edge_interior(vec4 weather, vec2 noises) {
	return smoothstep(0.46, 0.78, cloud_high_structure_field(weather, noises));
}

float cloud_low_vertical_profile(float h01, float type) {
	float h = clamp(h01, 0.0, 1.0);
	float stratus = smoothstep(0.00, 0.08, h)
		* (1.0 - smoothstep(0.74, 1.00, h));
	float cumulus = smoothstep(0.00, 0.12, h)
		* (1.0 - smoothstep(0.66, 1.00, h));
	cumulus *= 0.78 + 0.30 * sin(h * PI);
	float tower = smoothstep(0.00, 0.10, h)
		* (1.0 - smoothstep(0.84, 1.00, h));
	tower *= 0.78 + 0.34 * smoothstep(0.16, 0.72, h);

	float cumulus_mix = smoothstep(0.16, 0.54, type);
	float tower_mix = smoothstep(0.58, 0.88, type);
	return mix(mix(stratus, cumulus, cumulus_mix), tower, tower_mix);
}

float cloud_high_vertical_profile(float h01) {
	float h = clamp(h01, 0.0, 1.0);
	return smoothstep(0.0, 0.14, h) * (1.0 - smoothstep(0.66, 1.0, h));
}

vec2 cloud_macro_layers(vec4 weather, float altitude, vec2 low_noises, vec2 high_noises) {
	float low = 0.0;
	float high = 0.0;
	float type = cloud_type_from_weather(weather);
	float coverage = clamp(weather.r, 0.0, 1.0);
	float high_cov = clamp(weather.b, 0.0, 1.0);

	float low_base = cloud_base_from_weather(weather);
	float low_top = cloud_low_top_from_weather(weather);
	if (altitude >= low_base && altitude <= low_top) {
		float h = (altitude - low_base) / max(low_top - low_base, 1.0);
		float mask = cloud_low_structure_mask(weather, low_noises);
		low = mask * cloud_low_vertical_profile(h, type)
			* (0.50 + coverage * 0.34 + type * 0.42);
	}

	float high_base = cloud_high_base_from_weather(weather);
	float high_top = cloud_high_top_from_weather(weather);
	if (altitude >= high_base && altitude <= high_top && high_cov > 0.004) {
		float h = (altitude - high_base) / max(high_top - high_base, 1.0);
		float mask = cloud_high_structure_mask(weather, high_noises);
		high = mask * cloud_high_vertical_profile(h)
			* mix(0.10, 0.46, high_cov) * (0.80 + type * 0.20);
		high *= 1.0 - clamp(low * smoothstep(0.55, 0.90, type) * 0.38, 0.0, 0.42);
	}

	return vec2(low, high);
}

float cloud_macro_density(vec4 weather, float altitude, vec2 low_noises, vec2 high_noises) {
	vec2 layers = cloud_macro_layers(weather, altitude, low_noises, high_noises);
	return clamp(layers.x + layers.y, 0.0, 1.45);
}
