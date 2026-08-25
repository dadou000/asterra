shader_type spatial;
render_mode cull_disabled, diffuse_burley, specular_schlick_ggx;

uniform float u_grass_wind_strength = 0.70;
uniform float u_grass_wind_speed = 1.7;

varying vec3 v_grass_color;

void vertex() {
	// Compute has already supplied the final surface-aligned transform and scale.
	// INSTANCE_CUSTOM.rgb stores the classifier-derived colour; .a is phase noise.
	float tip = clamp(VERTEX.y, 0.0, 1.0);
	float phase = INSTANCE_CUSTOM.a * 6.28318530718;
	VERTEX.x += sin(TIME * u_grass_wind_speed + phase) * u_grass_wind_strength * tip * tip;
	v_grass_color = INSTANCE_CUSTOM.rgb;
}

void fragment() {
	ALBEDO = v_grass_color;
	ROUGHNESS = 0.96;
	SPECULAR = 0.08;
	METALLIC = 0.0;
}
