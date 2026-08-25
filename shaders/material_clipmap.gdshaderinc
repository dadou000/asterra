// Terrain material-control lookup.
//
// Primary path: one immutable six-face global RGBA8 texture loaded at world
// adoption. Legacy camera-centred clipmap uniforms remain as a fallback while old
// shaders/debug paths are retired, but normal movement never updates them.

uniform sampler2DArray u_material_global : filter_linear_mipmap, repeat_disable;
uniform float u_material_global_ready = 0.0;
uniform float u_material_global_res = 0.0;

uniform sampler2DArray u_material_clipmap : filter_linear, repeat_disable;
uniform float u_material_clipmap_ready = 0.0;
uniform vec3 u_material_center = vec3(1.0, 0.0, 0.0);
uniform vec3 u_material_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_material_up = vec3(0.0, 1.0, 0.0);
uniform float u_material_res = 128.0;
uniform vec3 u_material_texel_m = vec3(8.0, 128.0, 2048.0);


// Standalone copy of the cube-face addressing used by the planet surface lookup.
// Keeping it local avoids an include-order dependency for shaders that use only
// material_clipmap.gdshaderinc.
vec3 material_face_uv(vec3 dir) {
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


vec4 material_global_surface(vec3 planet_pos) {
	vec3 fuv = material_face_uv(normalize(planet_pos));
	ivec3 size3 = textureSize(u_material_global, 0);
	float texture_res = float(size3.x);
	float gutter = max((texture_res - u_material_global_res) * 0.5, 0.0);
	vec2 uv = (fuv.xy * u_material_global_res + vec2(gutter))
		/ max(texture_res, 1.0);
	return texture(u_material_global, vec3(uv, fuv.z));
}


vec4 material_layer(vec2 metres, float texel_m, float layer) {
	vec2 uv = metres / (texel_m * u_material_res) + vec2(0.5);
	return texture(u_material_clipmap, vec3(uv, layer));
}


vec4 material_clipmap_surface(vec3 planet_pos, float radius, vec4 fallback) {
	if (u_material_global_ready > 0.5 && u_material_global_res > 0.5) {
		return material_global_surface(planet_pos);
	}

	// Legacy fallback. This path should not run in the global terrain architecture.
	if (u_material_clipmap_ready < 0.5) return fallback;
	vec3 d = normalize(planet_pos);
	vec3 c = normalize(u_material_center);
	float denom = dot(d, c);
	if (denom <= 0.08) return fallback;
	vec2 metres = vec2(dot(d, normalize(u_material_right)),
		dot(d, normalize(u_material_up))) / denom * radius;
	float q0 = max(abs(metres.x), abs(metres.y))
		/ max(0.5 * u_material_res * u_material_texel_m.x, 1.0);
	float q1 = max(abs(metres.x), abs(metres.y))
		/ max(0.5 * u_material_res * u_material_texel_m.y, 1.0);
	float q2 = max(abs(metres.x), abs(metres.y))
		/ max(0.5 * u_material_res * u_material_texel_m.z, 1.0);
	if (q0 < 0.82) return material_layer(metres, u_material_texel_m.x, 0.0);
	if (q0 < 0.98) return mix(material_layer(metres, u_material_texel_m.x, 0.0),
		material_layer(metres, u_material_texel_m.y, 1.0), smoothstep(0.82, 0.98, q0));
	if (q1 < 0.82) return material_layer(metres, u_material_texel_m.y, 1.0);
	if (q1 < 0.98) return mix(material_layer(metres, u_material_texel_m.y, 1.0),
		material_layer(metres, u_material_texel_m.z, 2.0), smoothstep(0.82, 0.98, q1));
	if (q2 < 0.82) return material_layer(metres, u_material_texel_m.z, 2.0);
	if (q2 < 0.98) return mix(material_layer(metres, u_material_texel_m.z, 2.0),
		fallback, smoothstep(0.82, 0.98, q2));
	return fallback;
}
