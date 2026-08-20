// Shared cube-sphere lookup for orbit-quality surface textures.
// The CPU builds each face with a one-texel gutter, so linear filtering can
// cross a cube edge without exposing the texture-array layer boundary.

vec3 surface_face_uv(vec3 dir) {
	vec3 d = normalize(dir);
	vec3 ad = abs(d);
	int face = 0;
	vec3 axis = vec3(1.0, 0.0, 0.0);
	vec3 right = vec3(0.0, 0.0, -1.0);
	vec3 up = vec3(0.0, 1.0, 0.0);

	if (ad.x >= ad.y && ad.x >= ad.z) {
		if (d.x >= 0.0) {
			face = 0;
			axis = vec3(1.0, 0.0, 0.0);
			right = vec3(0.0, 0.0, -1.0);
			up = vec3(0.0, 1.0, 0.0);
		} else {
			face = 1;
			axis = vec3(-1.0, 0.0, 0.0);
			right = vec3(0.0, 0.0, 1.0);
			up = vec3(0.0, 1.0, 0.0);
		}
	} else if (ad.y >= ad.z) {
		if (d.y >= 0.0) {
			face = 2;
			axis = vec3(0.0, 1.0, 0.0);
			right = vec3(-1.0, 0.0, 0.0);
			up = vec3(0.0, 0.0, 1.0);
		} else {
			face = 3;
			axis = vec3(0.0, -1.0, 0.0);
			right = vec3(-1.0, 0.0, 0.0);
			up = vec3(0.0, 0.0, -1.0);
		}
	} else {
		if (d.z >= 0.0) {
			face = 4;
			axis = vec3(0.0, 0.0, 1.0);
			right = vec3(1.0, 0.0, 0.0);
			up = vec3(0.0, 1.0, 0.0);
		} else {
			face = 5;
			axis = vec3(0.0, 0.0, -1.0);
			right = vec3(-1.0, 0.0, 0.0);
			up = vec3(0.0, 1.0, 0.0);
		}
	}

	float denom = max(dot(axis, d), 1e-8);
	float q = PI * 0.25;
	float u = atan(dot(right, d) / denom) / q;
	float v = atan(dot(up, d) / denom) / q;
	return vec3(u * 0.5 + 0.5, v * 0.5 + 0.5, float(face));
}

vec3 surface_array_coord(vec3 dir, float face_res) {
	vec3 fuv = surface_face_uv(dir);
	float texture_res = face_res + 2.0;
	vec2 uv = (fuv.xy * face_res + vec2(1.0)) / texture_res;
	return vec3(uv, fuv.z);
}

// Explicit bilinear reconstruction for runtime-created texture arrays. This keeps
// the far/global coastline sub-texel smooth even if the material/project sampler
// state is nearest. The texture has a one-texel cross-face gutter, so clamping the
// four taps here remains seam-safe at cube edges.
float surface_macro_height(sampler2DArray tex, vec3 dir, float face_res) {
	vec3 coord = surface_array_coord(dir, face_res);
	ivec3 size3 = textureSize(tex, 0);
	vec2 size2 = vec2(size3.xy);
	vec2 p = coord.xy * size2 - vec2(0.5);
	ivec2 p0 = ivec2(floor(p));
	vec2 f = fract(p);
	ivec2 hi = size3.xy - ivec2(1);
	ivec2 a = clamp(p0, ivec2(0), hi);
	ivec2 b = clamp(p0 + ivec2(1, 0), ivec2(0), hi);
	ivec2 c = clamp(p0 + ivec2(0, 1), ivec2(0), hi);
	ivec2 e = clamp(p0 + ivec2(1, 1), ivec2(0), hi);
	int layer = int(coord.z + 0.5);
	float h00 = texelFetch(tex, ivec3(a, layer), 0).r;
	float h10 = texelFetch(tex, ivec3(b, layer), 0).r;
	float h01 = texelFetch(tex, ivec3(c, layer), 0).r;
	float h11 = texelFetch(tex, ivec3(e, layer), 0).r;
	return mix(mix(h00, h10, f.x), mix(h01, h11, f.x), f.y);
}

float surface_hash13(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.yzx + 33.33);
	return fract((p.x + p.y) * p.z);
}

float surface_vnoise(vec3 p) {
	vec3 i = floor(p);
	vec3 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);
	return mix(
		mix(mix(surface_hash13(i), surface_hash13(i + vec3(1.0, 0.0, 0.0)), f.x),
			mix(surface_hash13(i + vec3(0.0, 1.0, 0.0)), surface_hash13(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
		mix(mix(surface_hash13(i + vec3(0.0, 0.0, 1.0)), surface_hash13(i + vec3(1.0, 0.0, 1.0)), f.x),
			mix(surface_hash13(i + vec3(0.0, 1.0, 1.0)), surface_hash13(i + vec3(1.0, 1.0, 1.0)), f.x), f.y),
		f.z);
}

// The CPU texture already contains the real pristine TerrainDetail contribution
// around sea level. Do not add a second graphics-only displacement here: doing
// so invents islands that do not exist in the physical terrain and makes the
// coastline change when the distant representation takes over.
float surface_visual_height(sampler2DArray tex, vec3 dir, float face_res, float radius) {
	return surface_macro_height(tex, normalize(dir), face_res);
}
