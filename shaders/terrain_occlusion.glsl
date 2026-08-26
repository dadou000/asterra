#[compute]
#version 450

// Conservative terrain-ring occlusion test against Godot's resolved reverse-Z depth.
// Pass 0 initializes a coarse tile buffer, pass 1 reduces every scene-depth pixel
// into the farthest depth of its tile, and pass 2 tests conservative candidate
// spheres. A candidate is occluded only when every overlapped tile is entirely in
// front of the nearest point of its bounding sphere.
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D depth_texture;

layout(std430, set = 0, binding = 1) buffer TileDepthBuffer {
	uint min_reverse_z[];
} tiles;

layout(std430, set = 0, binding = 2) readonly buffer CandidateBuffer {
	// View-space xyz, conservative sphere radius in w. radius <= 0 means inactive.
	vec4 sphere[];
} candidates;

layout(std430, set = 0, binding = 3) buffer ResultBuffer {
	uint generation;
	uint candidate_count;
	uint valid;
	uint reserved;
	uint occluded[];
} results;

layout(push_constant, std430) uniform Params {
	// x = pass: 0 init, 1 depth reduction, 2 candidate test
	// y/z = full-resolution width/height
	// w = active candidate buffer count
	vec4 pass_screen_count;
	// x/y = conservative tile width/height
	// z = candidate generation
	// w = reverse-Z relative depth safety margin
	vec4 tile_generation_bias;
	mat4 projection;
} params;

const float CAMERA_EPSILON_M = 0.05;

uint float_bits(float value) {
	return floatBitsToUint(clamp(value, 0.0, 1.0));
}

void init_buffers() {
	uint idx = gl_GlobalInvocationID.x;
	uint tile_count = uint(max(params.tile_generation_bias.x, 1.0)
		* max(params.tile_generation_bias.y, 1.0));
	uint candidate_count = uint(max(params.pass_screen_count.w, 0.0));
	if (idx < tile_count) {
		// Reverse-Z: 1 is nearest. atomicMin during reduction therefore leaves the
		// farthest depth found in this coarse tile. Sky (0) makes the tile fail-open.
		tiles.min_reverse_z[idx] = float_bits(1.0);
	}
	if (idx < candidate_count) {
		results.occluded[idx] = 0u;
	}
	if (idx == 0u) {
		results.generation = uint(max(params.tile_generation_bias.z, 0.0) + 0.5);
		results.candidate_count = candidate_count;
		results.valid = 0u;
		results.reserved = 0u;
	}
}

void reduce_depth() {
	ivec2 screen_size = ivec2(max(params.pass_screen_count.yz, vec2(1.0)));
	ivec2 pixel = ivec2(int(gl_GlobalInvocationID.x), int(gl_GlobalInvocationID.y));
	if (pixel.x >= screen_size.x || pixel.y >= screen_size.y) return;

	ivec2 tile_size = ivec2(max(params.tile_generation_bias.xy, vec2(1.0)));
	ivec2 tile = ivec2(
		min(pixel.x * tile_size.x / screen_size.x, tile_size.x - 1),
		min(pixel.y * tile_size.y / screen_size.y, tile_size.y - 1));
	uint tile_index = uint(tile.y * tile_size.x + tile.x);
	float depth = texelFetch(depth_texture, pixel, 0).r;
	if (isnan(depth) || isinf(depth)) depth = 0.0;
	atomicMin(tiles.min_reverse_z[tile_index], float_bits(depth));
}

bool project_candidate(uint idx, out vec2 uv_min, out vec2 uv_max,
		out float candidate_nearest_depth) {
	vec4 s = candidates.sphere[idx];
	vec3 center = s.xyz;
	float radius = s.w;
	if (!(radius > 0.0) || any(isnan(center)) || any(isinf(center)) || !isfinite(radius)) {
		return false;
	}

	// Godot cameras look down -Z. If the conservative sphere reaches the camera
	// plane, never occlude it.
	if (center.z + radius >= -CAMERA_EPSILON_M) return false;

	vec2 ndc_min = vec2(1e20);
	vec2 ndc_max = vec2(-1e20);
	float nearest = 0.0;
	for (int z = 0; z < 2; z++) {
		for (int y = 0; y < 2; y++) {
			for (int x = 0; x < 2; x++) {
				vec3 sign_v = vec3(x == 0 ? -1.0 : 1.0,
					y == 0 ? -1.0 : 1.0,
					z == 0 ? -1.0 : 1.0);
				vec3 p = center + sign_v * radius;
				if (p.z >= -CAMERA_EPSILON_M) return false;
				vec4 clip = params.projection * vec4(p, 1.0);
				if (!(clip.w > 1e-8) || any(isnan(clip)) || any(isinf(clip))) return false;
				vec3 ndc = clip.xyz / clip.w;
				ndc_min = min(ndc_min, ndc.xy);
				ndc_max = max(ndc_max, ndc.xy);
				nearest = max(nearest, clamp(ndc.z, 0.0, 1.0));
			}
		}
	}

	// Completely offscreen candidates are not hidden here. Ordinary sector/frustum
	// culling already handles them, and fail-open avoids stale offscreen results.
	if (ndc_max.x < -1.0 || ndc_min.x > 1.0 || ndc_max.y < -1.0 || ndc_min.y > 1.0) {
		return false;
	}

	ndc_min = clamp(ndc_min, vec2(-1.0), vec2(1.0));
	ndc_max = clamp(ndc_max, vec2(-1.0), vec2(1.0));
	// Vulkan/Godot screen Y is inverted relative to NDC. Taking min/max after the
	// conversion handles that without a special branch.
	vec2 uv0 = vec2(ndc_min.x * 0.5 + 0.5, 0.5 - ndc_min.y * 0.5);
	vec2 uv1 = vec2(ndc_max.x * 0.5 + 0.5, 0.5 - ndc_max.y * 0.5);
	uv_min = min(uv0, uv1);
	uv_max = max(uv0, uv1);
	candidate_nearest_depth = nearest;
	return true;
}

void test_candidate() {
	uint idx = gl_GlobalInvocationID.x;
	uint count = uint(max(params.pass_screen_count.w, 0.0));
	if (idx >= count) return;

	vec2 uv_min;
	vec2 uv_max;
	float candidate_depth;
	if (!project_candidate(idx, uv_min, uv_max, candidate_depth)) {
		results.occluded[idx] = 0u;
		return;
	}

	ivec2 tile_size = ivec2(max(params.tile_generation_bias.xy, vec2(1.0)));
	ivec2 lo = clamp(ivec2(floor(uv_min * vec2(tile_size))), ivec2(0), tile_size - ivec2(1));
	ivec2 hi = clamp(ivec2(floor(uv_max * vec2(tile_size))), ivec2(0), tile_size - ivec2(1));

	// A relative margin is preferable to a fixed raw-depth epsilon because reverse-Z
	// precision is highly non-linear. Requiring the occluder to be at least ~2%
	// nearer makes self-depth/equality and temporal jitter fail open.
	float margin = max(params.tile_generation_bias.w, 0.0);
	float required_scene_depth = candidate_depth * (1.0 + margin) + 1e-8;
	for (int y = lo.y; y <= hi.y; y++) {
		for (int x = lo.x; x <= hi.x; x++) {
			float scene_farthest = uintBitsToFloat(tiles.min_reverse_z[y * tile_size.x + x]);
			if (!(scene_farthest > required_scene_depth)) {
				results.occluded[idx] = 0u;
				return;
			}
		}
	}
	results.occluded[idx] = 1u;
}

void main() {
	int pass_id = int(floor(params.pass_screen_count.x + 0.5));
	if (pass_id == 0) {
		init_buffers();
	} else if (pass_id == 1) {
		reduce_depth();
	} else if (pass_id == 2) {
		test_candidate();
		if (gl_GlobalInvocationID.x == 0u) results.valid = 1u;
	}
}
