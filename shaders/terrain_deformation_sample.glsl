#[compute]
#version 450

// Samples a small list of local positions from the active deformation field.
// This replaces whole-texture CPU readback: physics asks only for the handful of
// height/state points it actually needs, then asynchronously reads a tiny buffer.

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(rgba32f, set = 0, binding = 0) uniform readonly image2D state_field;

layout(set = 0, binding = 1, std430) restrict readonly buffer QueryBuffer {
	vec4 data[];
} queries;

layout(set = 0, binding = 2, std430) restrict writeonly buffer ResultBuffer {
	vec4 data[];
} results;

layout(set = 0, binding = 3, std140) uniform Params {
	// x=query count, y=resolution, z=spacing metres, w=half extent metres.
	vec4 values;
} params;

vec4 sample_bilinear(vec2 local_m, int resolution, float spacing_m) {
	vec2 f = local_m / spacing_m + vec2(float(resolution) * 0.5 - 0.5);
	if (f.x < 0.0 || f.y < 0.0 || f.x > float(resolution - 1) || f.y > float(resolution - 1)) {
		return vec4(0.0);
	}
	ivec2 p0 = ivec2(floor(f));
	ivec2 p1 = min(p0 + ivec2(1), ivec2(resolution - 1));
	vec2 t = fract(f);
	vec4 v00 = imageLoad(state_field, p0);
	vec4 v10 = imageLoad(state_field, ivec2(p1.x, p0.y));
	vec4 v01 = imageLoad(state_field, ivec2(p0.x, p1.y));
	vec4 v11 = imageLoad(state_field, p1);
	return mix(mix(v00, v10, t.x), mix(v01, v11, t.x), t.y);
}

void main() {
	uint index = gl_GlobalInvocationID.x;
	uint count = uint(clamp(int(params.values.x + 0.5), 0, 256));
	if (index >= count) {
		return;
	}
	int resolution = int(params.values.y + 0.5);
	float spacing_m = params.values.z;
	results.data[index] = sample_bilinear(queries.data[index].xy, resolution, spacing_m);
}
