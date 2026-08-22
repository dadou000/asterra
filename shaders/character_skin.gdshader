shader_type spatial;
render_mode diffuse_burley, specular_schlick_ggx, sss_mode_skin;

uniform sampler2D albedo_texture : source_color, filter_linear_mipmap_anisotropic, repeat_disable;
uniform vec4 skin_tone : source_color = vec4(0.92, 0.67, 0.52, 1.0);
uniform float tone_strength : hint_range(0.0, 1.0) = 0.62;
uniform float skin_roughness : hint_range(0.15, 0.95) = 0.52;
uniform float sss_strength : hint_range(0.0, 1.0) = 0.42;
uniform float pore_strength : hint_range(0.0, 1.0) = 0.22;

float hash21(vec2 p) {
	p = fract(p * vec2(123.34, 345.45));
	p += dot(p, p + 34.345);
	return fract(p.x * p.y);
}

void fragment() {
	vec3 source = texture(albedo_texture, UV).rgb;
	float luminance = dot(source, vec3(0.2126, 0.7152, 0.0722));
	vec3 source_detail = source / max(luminance, 0.08);
	float shaped_luminance = mix(0.48, 1.28, pow(clamp(luminance, 0.0, 1.0), 0.82));
	vec3 toned = skin_tone.rgb * shaped_luminance * mix(vec3(1.0), source_detail, 0.30);
	vec3 color = mix(source, toned, tone_strength);

	float pore = hash21(floor(UV * 2048.0));
	float micro_variation = (pore - 0.5) * pore_strength;
	ALBEDO = max(color, vec3(0.0));
	METALLIC = 0.0;
	SPECULAR = 0.34;
	ROUGHNESS = clamp(skin_roughness + micro_variation * 0.11 + (1.0 - luminance) * 0.05, 0.18, 0.92);
	SSS_STRENGTH = sss_strength * mix(0.72, 1.0, luminance);
	SSS_TRANSMITTANCE_COLOR = vec4(mix(vec3(1.0, 0.16, 0.08), skin_tone.rgb, 0.32), 1.0);
	SSS_TRANSMITTANCE_DEPTH = 0.018;
	SSS_TRANSMITTANCE_BOOST = 0.24;
	BACKLIGHT = skin_tone.rgb * sss_strength * 0.10;
}
