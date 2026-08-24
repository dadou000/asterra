#[compute]
#version 450

// Debug-only HDR luminance histogram. This runs at POST_TRANSPARENT, before
// Godot's built-in exposure/tonemap pass, so the bins describe scene-linear
// luminance in the domain that matters for eye adaptation.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform readonly image2D color_image;

layout(std430, set = 0, binding = 1) buffer HistogramBuffer {
	uint bins[64];
	uint total_count;
	uint below_range_count;
	uint above_range_count;
	uint invalid_count;
} histogram;

layout(push_constant, std430) uniform Params {
	// x = pixel sampling stride
	// y = minimum EV relative to middle grey
	// z = maximum EV relative to middle grey
	// w = middle-grey scene-linear luminance
	vec4 capture;
} params;

const vec3 REC709_LUMA = vec3(0.2126, 0.7152, 0.0722);
const int BIN_COUNT = 64;

void main() {
	ivec2 image_size = imageSize(color_image);
	int stride = max(int(floor(params.capture.x + 0.5)), 1);
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy) * stride + ivec2(stride / 2);
	if (pixel.x >= image_size.x || pixel.y >= image_size.y) {
		return;
	}

	vec3 hdr = imageLoad(color_image, pixel).rgb;
	if (any(isnan(hdr)) || any(isinf(hdr))) {
		atomicAdd(histogram.invalid_count, 1u);
		return;
	}

	float luminance = max(dot(max(hdr, vec3(0.0)), REC709_LUMA), 1.0e-12);
	float middle_grey = max(params.capture.w, 1.0e-6);
	float ev = log2(luminance / middle_grey);
	float min_ev = params.capture.y;
	float max_ev = max(params.capture.z, min_ev + 0.001);

	atomicAdd(histogram.total_count, 1u);

	if (ev < min_ev) {
		atomicAdd(histogram.below_range_count, 1u);
		atomicAdd(histogram.bins[0], 1u);
		return;
	}
	if (ev >= max_ev) {
		atomicAdd(histogram.above_range_count, 1u);
		atomicAdd(histogram.bins[BIN_COUNT - 1], 1u);
		return;
	}

	float normalized = (ev - min_ev) / (max_ev - min_ev);
	int bin_index = clamp(int(floor(normalized * float(BIN_COUNT))), 0, BIN_COUNT - 1);
	atomicAdd(histogram.bins[bin_index], 1u);
}
