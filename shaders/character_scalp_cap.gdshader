shader_type spatial;
render_mode depth_prepass_alpha, cull_back, diffuse_burley, specular_schlick_ggx;

// The coverage layer beneath the hair cards. Cards cannot tile a curved scalp,
// so something has to sit in the gaps — but a flat coloured shell reads as a
// painted mask the moment a lock parts. This samples the same baked strip the
// cards do, along the same comb field, so what shows through the parting is
// hair roots rather than a smooth brown patch.
uniform sampler2D hair_strip : source_color, hint_default_transparent, filter_linear_mipmap_anisotropic, repeat_enable;
uniform float strip_variants = 3.0;
uniform vec4 root_color : source_color = vec4(0.12, 0.055, 0.025, 1.0);
uniform float sheen : hint_range(0.0, 1.0) = 0.55;
uniform float sheen_roughness : hint_range(0.05, 1.0) = 0.52;

// Triangle wave. Mirroring the strip instead of repeating it keeps the tiling
// continuous, so no seam appears where a tile would otherwise wrap from feathered
// tips back onto solid roots.
float ping_pong(float value) {
	return abs(fract(value * 0.5) * 2.0 - 1.0);
}

void fragment() {
	// Only the dense root band of the strip is used: this layer is roots, not
	// full-length strands.
	vec2 strip_uv = vec2(ping_pong(UV.x) / strip_variants, ping_pong(UV.y) * 0.30);
	vec4 strip = texture(hair_strip, strip_uv);

	float feather = COLOR.a;
	vec3 fiber_color = root_color.rgb * (0.80 + 0.40 * strip.r);
	ALBEDO = fiber_color * mix(0.60, 1.0, strip.g);
	ROUGHNESS = sheen_roughness + 0.10;
	SPECULAR = sheen * 0.20 * mix(0.45, 1.0, strip.g);
	ANISOTROPY = sheen * 0.6;
	// TANGENT comes from the mesh and points along the comb, so this highlight
	// runs the same way as the one on the cards above.
	ANISOTROPY_FLOW = normalize(vec2(1.0, (strip.b - 0.5) * 0.40));
	BACKLIGHT = fiber_color * (0.30 + 0.25 * sheen);

	// Solid well inside the hairline, breaking into individual strands at the
	// edge. A smooth alpha ramp here is what makes a cap look like a swim cap.
	ALPHA = max(
		smoothstep(0.34, 0.72, feather),
		clamp(strip.a * 1.2, 0.0, 1.0) * smoothstep(-0.02, 0.42, feather)
	);
}
