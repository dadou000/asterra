shader_type spatial;
render_mode unshaded, blend_add, cull_disabled, depth_draw_never;

// HDR emissive lightning ribbons. Geometry is generated procedurally by
// CloudRenderer in render space while the bolt itself is stored in canonical
// planet coordinates, so floating-origin rebases cannot detach a flash from a
// storm.
uniform float u_emission = 7.5;

void fragment() {
	vec3 c = COLOR.rgb;
	ALBEDO = vec3(0.0);
	EMISSION = c * u_emission;
	ALPHA = COLOR.a;
}
