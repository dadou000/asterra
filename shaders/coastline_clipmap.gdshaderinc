// Camera-centred coastline clipmap shared by terrain, local ocean and orbit ocean.
// All three representations sample the same planet-space height field, so changing
// terrain/water LOD cannot move the visible shoreline.

uniform sampler2DArray u_coast_clipmap : filter_linear, repeat_disable;
uniform float u_coast_clipmap_ready = 0.0;
uniform vec3 u_coast_center = vec3(1.0, 0.0, 0.0);
uniform vec3 u_coast_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_coast_up = vec3(0.0, 1.0, 0.0);
uniform float u_coast_res = 512.0;
uniform vec3 u_coast_texel_m = vec3(35.0, 120.0, 480.0);

float coast_layer_height(vec2 metres, float texel_m, float layer) {
	vec2 uv = metres / (texel_m * u_coast_res) + vec2(0.5);
	return texture(u_coast_clipmap, vec3(uv, layer)).r;
}

// 1 where the camera-centred clipmap is authoritative, fading to 0 over the
// outermost ring. Local water uses this to fall back to its own signed mesh depth
// instead of the much coarser whole-planet texture. That matters near the ground:
// the global texture is intentionally an orbit representation and can differ by
// hundreds of metres from runtime TerrainDetail at a coastline.
float coast_clipmap_coverage(vec3 dir, float radius) {
	if (u_coast_clipmap_ready < 0.5) {
		return 0.0;
	}
	vec3 d = normalize(dir);
	vec3 c = normalize(u_coast_center);
	float denom = dot(d, c);
	if (denom <= 0.08) {
		return 0.0;
	}
	vec2 metres = vec2(dot(d, normalize(u_coast_right)), dot(d, normalize(u_coast_up)))
		/ denom * radius;
	float half2 = 0.5 * u_coast_res * u_coast_texel_m.z;
	float q2 = max(abs(metres.x), abs(metres.y)) / max(half2, 1.0);
	return 1.0 - smoothstep(0.82, 0.98, q2);
}

float coast_surface_height(sampler2DArray fallback_tex, vec3 dir, float fallback_face_res,
		float radius) {
	vec3 d = normalize(dir);
	float fallback_h = surface_visual_height(fallback_tex, d, fallback_face_res, radius);
	if (u_coast_clipmap_ready < 0.5) {
		return fallback_h;
	}

	// Gnomonic projection: this is the exact inverse of the CPU's
	// normalize(center + right*x/R + up*y/R) sampling map.
	float denom = dot(d, normalize(u_coast_center));
	if (denom <= 0.08) {
		return fallback_h;
	}
	vec2 metres = vec2(dot(d, normalize(u_coast_right)), dot(d, normalize(u_coast_up)))
		/ denom * radius;

	float half0 = 0.5 * u_coast_res * u_coast_texel_m.x;
	float half1 = 0.5 * u_coast_res * u_coast_texel_m.y;
	float half2 = 0.5 * u_coast_res * u_coast_texel_m.z;
	float q0 = max(abs(metres.x), abs(metres.y)) / max(half0, 1.0);
	float q1 = max(abs(metres.x), abs(metres.y)) / max(half1, 1.0);
	float q2 = max(abs(metres.x), abs(metres.y)) / max(half2, 1.0);

	if (q0 < 0.82) {
		return coast_layer_height(metres, u_coast_texel_m.x, 0.0);
	}
	if (q0 < 0.98) {
		float a = smoothstep(0.82, 0.98, q0);
		float h0 = coast_layer_height(metres, u_coast_texel_m.x, 0.0);
		float h1 = coast_layer_height(metres, u_coast_texel_m.y, 1.0);
		return mix(h0, h1, a);
	}

	if (q1 < 0.82) {
		return coast_layer_height(metres, u_coast_texel_m.y, 1.0);
	}
	if (q1 < 0.98) {
		float a = smoothstep(0.82, 0.98, q1);
		float h1 = coast_layer_height(metres, u_coast_texel_m.y, 1.0);
		float h2 = coast_layer_height(metres, u_coast_texel_m.z, 2.0);
		return mix(h1, h2, a);
	}

	if (q2 < 0.82) {
		return coast_layer_height(metres, u_coast_texel_m.z, 2.0);
	}
	if (q2 < 0.98) {
		float a = smoothstep(0.82, 0.98, q2);
		float h2 = coast_layer_height(metres, u_coast_texel_m.z, 2.0);
		return mix(h2, fallback_h, a);
	}
	return fallback_h;
}
