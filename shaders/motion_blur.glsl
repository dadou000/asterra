#[compute]
#version 450

// Camera-reprojection motion blur against Godot's resolved reverse-Z depth.
//
// Each pixel's current-frame NDC + depth is reprojected into the PREVIOUS frame's
// clip space using a single combined matrix built on the CPU from this frame's and
// last frame's camera transform/projection (see MotionBlurCompositorEffect). The
// screen-space delta between the two is the blur direction and length. This is
// camera-motion blur: it is correct for the dominant case (the camera flying/
// walking through the world) but does not add extra streak for a fast independent
// dynamic actor (e.g. a vehicle) beyond what its screen-space position implies.
//
// Godot's resolved color render target has STORAGE_BIT + SAMPLING_BIT but NOT the
// copy usage bits texture_copy() needs, so it cannot be duplicated with a driver
// blit. Pass 0 instead copies it into a scratch texture via imageLoad/imageStore
// (both textures support that); pass 1 samples the untouched scratch copy for the
// blur taps and writes the result back into the resolved color image in place.
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform image2D color_image;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(rgba16f, set = 0, binding = 2) uniform image2D scratch_image;
layout(set = 0, binding = 3) uniform sampler2D scratch_sampler;

layout(std430, set = 0, binding = 4) readonly buffer ReprojectBuffer {
	// Maps this frame's clip-space (ndc.xy, reverse-Z depth, 1) directly to the
	// previous frame's clip space.
	mat4 prev_clip_from_current_clip;
	float has_prev;
	float pad0;
	float pad1;
	float pad2;
} reproject;

layout(push_constant, std430) uniform Params {
	vec2 screen_size;
	float strength;
	float max_blur_px;
	float sample_count;
	float pass_id; // 0 = copy color -> scratch, 1 = blur scratch -> color
	float pad1;
	float pad2;
} params;

vec2 ndc_to_uv(vec2 ndc) {
	// Vulkan/Godot screen Y is inverted relative to NDC (matches terrain_occlusion.glsl).
	return vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

void copy_pass(ivec2 pixel) {
	imageStore(scratch_image, pixel, imageLoad(color_image, pixel));
}

void blur_pass(ivec2 pixel) {
	vec2 uv = (vec2(pixel) + 0.5) / params.screen_size;
	vec4 center_color = texture(scratch_sampler, uv);

	if (reproject.has_prev < 0.5) {
		imageStore(color_image, pixel, center_color);
		return;
	}

	// Reverse-Z: 0 is the far plane / sky, where no world-space point exists to
	// reproject. Leaving the sky sharp is deliberate for now.
	float depth = texelFetch(depth_texture, pixel, 0).r;
	if (depth <= 0.0) {
		imageStore(color_image, pixel, center_color);
		return;
	}

	vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
	vec4 prev_clip = reproject.prev_clip_from_current_clip * vec4(ndc, depth, 1.0);
	if (!(prev_clip.w > 1e-6) || any(isnan(prev_clip)) || any(isinf(prev_clip))) {
		imageStore(color_image, pixel, center_color);
		return;
	}
	vec2 prev_uv = ndc_to_uv(prev_clip.xy / prev_clip.w);

	vec2 velocity_px = (uv - prev_uv) * params.screen_size;
	float speed = length(velocity_px);
	// The max-pixel clamp is also what keeps a one-frame discontinuity (a teleport,
	// a body-swap rebase edge case) from ever smearing the whole screen: however
	// large the raw reprojected velocity is, the kernel radius stays bounded.
	float blur_px = min(speed * params.strength, params.max_blur_px);
	if (blur_px < 0.6) {
		imageStore(color_image, pixel, center_color);
		return;
	}

	vec2 dir = velocity_px / max(speed, 1e-6);
	float sample_count = max(params.sample_count, 2.0);
	vec2 step_uv = dir * (blur_px / params.screen_size) / max(sample_count - 1.0, 1.0);

	// Interleaved-gradient-noise dither on the sample offset turns banding from a
	// fixed per-pixel-independent tap pattern into cheap, unobtrusive noise.
	float dither = fract(52.9829189 * fract(dot(vec2(pixel), vec2(0.06711056, 0.00583715))));

	vec4 accum = vec4(0.0);
	float half_count = (sample_count - 1.0) * 0.5;
	for (float i = 0.0; i < sample_count; i += 1.0) {
		float t = (i - half_count) + (dither - 0.5);
		vec2 sample_uv = clamp(uv + step_uv * t, vec2(0.0), vec2(1.0));
		accum += texture(scratch_sampler, sample_uv);
	}
	imageStore(color_image, pixel, accum / sample_count);
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = ivec2(params.screen_size);
	if (pixel.x >= size.x || pixel.y >= size.y) return;

	if (int(params.pass_id + 0.5) == 0) {
		copy_pass(pixel);
	} else {
		blur_pass(pixel);
	}
}
