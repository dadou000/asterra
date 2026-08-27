#[compute]
#version 450

// Active high-resolution terrain deformation tile.
// One invocation owns one texel, so no atomics are required. Contacts are batched
// on the CPU into a tiny buffer and every texel evaluates the same contact list.
// RGBA state: R=height delta, G=compaction, B=shear/plastic damage, A=reserved.

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba32f, set = 0, binding = 0) uniform readonly image2D src_state;
layout(rgba32f, set = 0, binding = 1) uniform writeonly image2D dst_state;

layout(set = 0, binding = 2, std430) restrict readonly buffer ContactBuffer {
	vec4 data[];
} contacts;

layout(set = 0, binding = 3, std140) uniform Params {
	// x = contact count, y = resolution, z = sample spacing metres, w = half extent metres.
	vec4 values;
} params;

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	int resolution = int(params.values.y + 0.5);
	if (pixel.x >= resolution || pixel.y >= resolution) {
		return;
	}

	vec4 state = imageLoad(src_state, pixel);
	float spacing_m = params.values.z;
	vec2 local_m = (vec2(pixel) + vec2(0.5) - vec2(float(resolution) * 0.5)) * spacing_m;
	int contact_count = clamp(int(params.values.x + 0.5), 0, 64);

	for (int contact_index = 0; contact_index < contact_count; ++contact_index) {
		vec4 a = contacts.data[contact_index * 2 + 0];
		vec4 b = contacts.data[contact_index * 2 + 1];
		vec2 contact_center_m = a.xy;
		float radius_m = max(a.z, spacing_m * 0.5);
		float sink_m = max(a.w, 0.0);
		float compaction_gain = max(b.x, 0.0);
		float damage_gain = max(b.y, 0.0);
		float rim_fraction = clamp(b.z, 0.0, 0.95);
		float max_depth_m = max(b.w, 0.05);

		float distance_m = length(local_m - contact_center_m);
		if (distance_m <= radius_m) {
			float q = distance_m / radius_m;
			float weight = 1.0 - q * q;
			weight *= weight;
			float requested = -sink_m * weight;
			float before = state.r;
			state.r = clamp(state.r + requested, -max_depth_m, max_depth_m * 0.25);
			float actual_removed = max(before - state.r, 0.0);
			state.g = clamp(state.g + actual_removed * compaction_gain, 0.0, 1.0);
			state.b = clamp(state.b + actual_removed * damage_gain, 0.0, 1.0);
		}

		// Continuous volume-normalized rim. The depression kernel integrates to
		// PI*R^2/3. For the triangular 1.08R..1.90R ring, this coefficient puts
		// approximately rim_fraction of that displaced volume around the crater.
		if (rim_fraction > 0.0) {
			float ring_inner = radius_m * 1.08;
			float ring_outer = radius_m * 1.90;
			if (distance_m >= ring_inner && distance_m <= ring_outer) {
				float ring_mid = (ring_inner + ring_outer) * 0.5;
				float ring_half_width = max((ring_outer - ring_inner) * 0.5, spacing_m);
				float ring_weight = max(1.0 - abs(distance_m - ring_mid) / ring_half_width, 0.0);
				float rim_peak_m = sink_m * rim_fraction * 0.273;
				state.r = clamp(state.r + rim_peak_m * ring_weight,
					-max_depth_m, max_depth_m * 0.25);
			}
		}
	}

	imageStore(dst_state, pixel, state);
}
