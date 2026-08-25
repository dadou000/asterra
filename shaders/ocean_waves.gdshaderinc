// Shared finite-depth ocean wave model for rendering and GPU physics.
//
// The physical wave solution is world-space invariant. Render LOD is allowed to
// decide only how much of a wavelength can be represented by a particular mesh;
// it must never alter phase, direction, dispersion frequency or Gerstner shape.
// In shallow water propagation bends toward the seabed gradient, crests wrap
// around coasts, and breaking is bounded by H ~= gamma*d (gamma ~0.78).

const float OCEAN_G = 9.81;
const float BREAKER_GAMMA = 0.78;

struct OceanWaveSample {
	vec3 displacement;
	vec3 slope;
	vec3 velocity;
	float crest;
	float breaking;
	float compression;
};

vec3 ocean_safe_tangent(vec3 d, vec3 candidate) {
	vec3 t = candidate - d * dot(candidate, d);
	float l2 = dot(t, t);
	if (l2 < 1e-8) {
		vec3 axis = abs(d.y) < 0.85 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		t = cross(axis, d);
		l2 = max(dot(t, t), 1e-8);
	}
	return t * inversesqrt(l2);
}

float ocean_wave_band_weight(float wavelength, float spacing) {
	// At least ~7 vertices/wavelength at full strength; smoothly remove waves as
	// a clipmap level becomes too coarse to carry them. This is a representation
	// weight only. It is deliberately applied after the physical wave is solved.
	return 1.0 - smoothstep(wavelength / 10.0, wavelength / 5.5, spacing);
}

float ocean_dispersion(float k, float depth) {
	return sqrt(max(OCEAN_G * k * tanh(k * max(depth, 0.05)), 1e-5));
}

float ocean_shoal_gain(float depth, float wavelength) {
	float q = clamp(depth / max(wavelength * 0.20, 0.1), 0.0, 1.0);
	return mix(1.42, 1.0, smoothstep(0.08, 0.75, q));
}

float ocean_coast_frequency_scale(float depth, float wavelength) {
	// Finite-depth dispersion already slows waves as the seabed rises. This extra
	// surf-zone term deliberately removes the remaining fast oscillation that can
	// make game-ocean beaches look like boiling water. Deep water is unchanged.
	float transition_depth = max(12.0, wavelength * 0.32);
	float deep_blend = smoothstep(0.75, transition_depth, depth);
	return mix(0.56, 1.0, deep_blend);
}

float ocean_diffraction_weight(vec3 up, vec3 incident, vec3 landward, float depth) {
	vec3 i = ocean_safe_tangent(up, incident);
	vec3 l = ocean_safe_tangent(up, landward);
	float oblique = 1.0 - abs(dot(i, l));
	float coastal = 1.0 - smoothstep(12.0, 170.0, depth);
	return coastal * smoothstep(0.12, 0.88, oblique);
}

vec3 ocean_diffraction_direction(vec3 up, vec3 incident, vec3 landward) {
	vec3 l = ocean_safe_tangent(up, landward);
	vec3 coast_tangent = normalize(cross(up, l));
	float side = dot(ocean_safe_tangent(up, incident), coast_tangent) >= 0.0 ? 1.0 : -1.0;
	// Huygens-style approximation: energy spreads around the coast from the side
	// on which the incident wave arrives instead of snapping directly shoreward.
	return ocean_safe_tangent(up, l + coast_tangent * side * 0.72);
}

void ocean_add_wave(vec3 up, vec3 dir, float offshore_phase_coord,
		float signed_shore_distance, float depth, float spacing, float wavelength,
		float base_amp, float steepness, float time_s, float phase_offset,
		inout OceanWaveSample out_wave) {
	float lod_w = ocean_wave_band_weight(wavelength, spacing);
	if (lod_w <= 0.001 || depth <= 0.01) return;

	float k = TAU / wavelength;
	float omega = ocean_dispersion(k, depth) * ocean_coast_frequency_scale(depth, wavelength);

	// Solve the physical wave before considering mesh LOD. In the old path lod_w
	// reduced amp before q was calculated. q then rose as amp fell, which kept much
	// of the horizontal Gerstner displacement alive while the vertical component
	// vanished. Adjacent clipmap levels therefore represented different trochoids.
	float raw_amp = base_amp * ocean_shoal_gain(depth, wavelength);
	float breaker_amp = max(0.015, BREAKER_GAMMA * depth * 0.5);
	float break_ratio = raw_amp / max(breaker_amp, 0.015);
	float amp = min(raw_amp, breaker_amp);

	vec3 travel = ocean_safe_tangent(up, dir);
	// The offshore coordinate is supplied from a fixed planet-space phase axis.
	// It therefore has no dependency on clipmap centre, ring number or tessellation.
	float shore_blend = (1.0 - smoothstep(18.0, 150.0, depth)) * 0.94;
	float phase_coord = mix(offshore_phase_coord, signed_shore_distance, shore_blend);
	float phase = phase_coord * k - omega * time_s + phase_offset;

	float s = sin(phase);
	float c = cos(phase);
	float q = min(steepness / max(k * max(amp, 0.01), 0.01), 0.95);
	float horizontal = q * amp;

	// LOD is applied to the complete solved wave. Thus a fine ring approaching its
	// parent becomes exactly the same wave field as that parent at the handoff.
	out_wave.displacement += (up * (amp * s) + travel * (horizontal * c)) * lod_w;
	out_wave.slope += travel * (amp * k * c) * lod_w;
	out_wave.velocity += (up * (-amp * omega * c) + travel * (horizontal * omega * s)) * lod_w;
	out_wave.crest = max(out_wave.crest, (s * 0.5 + 0.5) * lod_w);
	out_wave.breaking = max(out_wave.breaking,
		smoothstep(0.72, 1.08, break_ratio)
		* (1.0 - smoothstep(0.3, 8.0, depth)) * lod_w);
	// Horizontal Gerstner compression peaks close to the crest and is a useful
	// stateless proxy for whitecap generation.
	out_wave.compression = max(out_wave.compression,
		clamp(max(s, 0.0) * horizontal * k * 2.4 * lod_w, 0.0, 1.0));
}

OceanWaveSample ocean_empty_wave() {
	OceanWaveSample wave;
	wave.displacement = vec3(0.0);
	wave.slope = vec3(0.0);
	wave.velocity = vec3(0.0);
	wave.crest = 0.0;
	wave.breaking = 0.0;
	wave.compression = 0.0;
	return wave;
}
