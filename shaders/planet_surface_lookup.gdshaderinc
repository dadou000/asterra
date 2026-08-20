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

float surface_macro_height(sampler2DArray tex, vec3 dir, float face_res) {
	return texture(tex, surface_array_coord(dir, face_res)).r;
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

// Orbit coastline detail is intentionally graphics-only. It perturbs the zero
// contour by a small amount in planet-space, which gives the coarse macro field
// a crisp, irregular shoreline without making islands appear far from sea level.
float surface_visual_height(sampler2DArray tex, vec3 dir, float face_res, float radius) {
	vec3 up = normalize(dir);
	float macro_h = surface_macro_height(tex, up, face_res);
	float gate = 1.0 - smoothstep(650.0, 1800.0, abs(macro_h));
	vec3 p = up * radius;
	float n0 = surface_vnoise(p * 0.00014) - 0.5;
	float n1 = surface_vnoise(p * 0.00046 + vec3(17.0, -9.0, 23.0)) - 0.5;
	float perturb = n0 * 170.0 + n1 * 52.0;
	return macro_h + perturb * gate;
}
