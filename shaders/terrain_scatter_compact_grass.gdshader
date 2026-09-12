shader_type spatial;
render_mode cull_disabled, diffuse_burley, specular_schlick_ggx;

// Same global wind field as gpu_scatter_common.gdshaderinc (see that file for
// the shared version's comments); declared locally here rather than pulling
// in the full scatter include, since this path is currently disabled
// (gpu_terrain_scatter_compact.gd's STABLE_FALLBACK_ONLY) and has no
// per-instance right/north tangent basis to bend along a real wind direction
// -- only local X is available here.
global uniform float u_global_wind_speed = 1.6;
global uniform float u_global_wind_strength = 1.0;

uniform float u_grass_wind_strength = 0.70;

varying vec3 v_grass_color;

void vertex() {
	// Compute has already supplied the final surface-aligned transform and scale.
	// INSTANCE_CUSTOM.rgb stores the classifier-derived colour; .a is phase noise.
	float height01 = clamp(VERTEX.y, 0.0, 1.0);
	float phase = INSTANCE_CUSTOM.a * 6.28318530718;
	float primary = sin(TIME * u_global_wind_speed + phase);
	float secondary = sin(TIME * u_global_wind_speed * 1.73 + phase * 1.7) * 0.35;
	float gust = sin(TIME * u_global_wind_speed * 0.41 + phase * 0.3) * 0.2;
	float height_factor = mix(0.05, 1.0, smoothstep(0.0, 1.0, height01));
	VERTEX.x += (primary + secondary + gust) * u_global_wind_strength
		* u_grass_wind_strength * height_factor;
	v_grass_color = INSTANCE_CUSTOM.rgb;
}

void fragment() {
	ALBEDO = v_grass_color;
	ROUGHNESS = 0.96;
	SPECULAR = 0.08;
	METALLIC = 0.0;
}
