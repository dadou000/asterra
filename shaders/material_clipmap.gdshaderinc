// Camera-centred virtual material-control texture. It decouples biome/material
// classification from visual mesh vertices, so a quadtree LOD transition cannot
// enlarge, rotate, or erase grass/soil/forest weights.

uniform sampler2DArray u_material_clipmap : filter_linear, repeat_disable;
uniform float u_material_clipmap_ready = 0.0;
uniform vec3 u_material_center = vec3(1.0, 0.0, 0.0);
uniform vec3 u_material_right = vec3(0.0, 0.0, -1.0);
uniform vec3 u_material_up = vec3(0.0, 1.0, 0.0);
uniform float u_material_res = 128.0;
uniform vec3 u_material_texel_m = vec3(8.0, 128.0, 2048.0);

vec4 material_layer(vec2 metres, float texel_m, float layer) {
	vec2 uv = metres / (texel_m * u_material_res) + vec2(0.5);
	return texture(u_material_clipmap, vec3(uv, layer));
}

vec4 material_clipmap_surface(vec3 planet_pos, float radius, vec4 fallback) {
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
