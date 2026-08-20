/// Shared large-scale cloud morphology.
///
/// The weather simulation owns cloud geography. Procedural noise only perturbs
/// the *edge* of that geography; it never decides cloud coverage by itself.
/// This keeps orbit, volume rendering and projected shadows on one coherent mask
/// and avoids the obvious repetition produced by tiling a 3D noise texture over
/// continental distances.

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

/// Two seamless direction-space scales. Because the input is the normalized
/// planet direction there are no cube-face seams and no texture wrap period.
vec2 cloud_structure_noises(vec3 dir) {
	vec3 d = normalize(dir);
	float synoptic = cloud_struct_vnoise(d * 11.0 + vec3(7.1, -3.4, 5.7));
	float mesoscale = cloud_struct_vnoise(d * 37.0 + vec3(-13.2, 8.6, 21.4));
	return vec2(synoptic, mesoscale);
}

/// Signed low-cloud structure field. 0 is outside; values around 0.5 are the
/// edge. Weather coverage remains the dominant term. The perturbation fades in
/// overcast so a solid stratiform deck does not turn into a field of repeated
/// holes.
float cloud_low_structure_field(vec4 weather, vec2 noises) {
	float coverage = clamp(weather.r, 0.0, 1.0);
	float convective = clamp(weather.g, 0.0, 1.0);
	float edge_freedom = 1.0 - smoothstep(0.70, 0.94, coverage);
	float perturb = (noises.x - 0.5) * 0.23 + (noises.y - 0.5) * 0.075;
	return coverage + perturb * edge_freedom + convective * 0.055;
}

float cloud_high_structure_field(vec4 weather, vec2 noises) {
	float high_cov = clamp(weather.b, 0.0, 1.0);
	float convective = clamp(weather.g, 0.0, 1.0);
	float edge_freedom = 1.0 - smoothstep(0.62, 0.90, high_cov);
	float perturb = (noises.x - 0.5) * 0.18 + (noises.y - 0.5) * 0.10;
	return high_cov + perturb * edge_freedom + convective * 0.08;
}

float cloud_low_structure_mask(vec4 weather, vec2 noises) {
	return smoothstep(0.15, 0.49, cloud_low_structure_field(weather, noises));
}

float cloud_high_structure_mask(vec4 weather, vec2 noises) {
	return smoothstep(0.10, 0.43, cloud_high_structure_field(weather, noises));
}

/// 0 at the macro edge, 1 in the interior. Used as an inexpensive SDF-like
/// control for detail erosion. It is intentionally not a metric distance: the
/// weather grid supplies the large-scale signed field, while fine erosion is
/// only needed in a narrow visual band around the boundary.
float cloud_low_edge_interior(vec4 weather, vec2 noises) {
	float f = cloud_low_structure_field(weather, noises);
	return smoothstep(0.49, 0.78, f);
}

float cloud_high_edge_interior(vec4 weather, vec2 noises) {
	float f = cloud_high_structure_field(weather, noises);
	return smoothstep(0.43, 0.72, f);
}

/// Continuous cloud-type selector. The weather simulation's convective
/// potential acts as a dynamic type map: 0=stratiform, ~0.4=cumulus,
/// ~0.7=towering cumulus, 1=cumulonimbus.
float cloud_type_from_weather(vec4 weather) {
	return clamp(weather.g, 0.0, 1.0);
}
