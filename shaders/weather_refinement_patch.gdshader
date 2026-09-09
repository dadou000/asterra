shader_type spatial;
render_mode unshaded, blend_mix, depth_draw_never, cull_disabled;

uniform vec4 u_color : source_color = vec4(0.3, 0.9, 0.5, 0.85);

void fragment() {
	ALBEDO = vec3(0.0);
	EMISSION = u_color.rgb * (0.65 + 0.55 * u_color.a);
	ALPHA = u_color.a;
}
