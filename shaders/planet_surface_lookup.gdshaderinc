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
