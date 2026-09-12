shader_type canvas_item;
render_mode unshaded;

uniform sampler2D screen_texture : hint_screen_texture, filter_linear_mipmap;
uniform float u_rain : hint_range(0.0, 1.0) = 0.0;
uniform float u_wetness : hint_range(0.0, 1.0) = 0.0;
uniform float u_speed = 0.0;
uniform float u_lightning_flash : hint_range(0.0, 1.0) = 0.0;
uniform float u_time = 0.0;

float hash21(vec2 p) {
	p = fract(p * vec2(123.34, 456.21));
	p += dot(p, p + 45.32);
	return fract(p.x * p.y);
}

vec2 hash22(vec2 p) {
	float n = hash21(p);
	return vec2(n, hash21(p + n + 17.17));
}

float droplet_field(vec2 uv, float scale, float time, out vec2 normal_offset) {
	vec2 grid = uv * scale;
	vec2 id = floor(grid);
	vec2 f = fract(grid) - 0.5;
	vec2 rnd = hash22(id);
	float phase = fract(rnd.x + time * mix(0.015, 0.08, u_rain));
	float streak = mix(0.0, 0.38, clamp(u_speed / 35.0, 0.0, 1.0));
	vec2 centre = vec2((rnd.x - 0.5) * 0.55,
		0.42 - phase * (1.10 + streak));
	vec2 d = f - centre;
	d.y *= mix(1.0, 0.42, streak);
	float radius = mix(0.055, 0.16, rnd.y) * mix(0.65, 1.15, u_wetness);
	float drop = 1.0 - smoothstep(radius * 0.72, radius, length(d));
	float rim = smoothstep(radius * 0.40, radius * 0.88, length(d)) * drop;
	normal_offset = normalize(d + vec2(1e-5)) * rim * radius * 0.055;
	return drop;
}

void fragment() {
	vec2 uv = SCREEN_UV;
	vec2 n0;
	vec2 n1;
	float d0 = droplet_field(uv, 13.0, u_time, n0);
	float d1 = droplet_field(uv + vec2(0.137, 0.271), 23.0, u_time * 0.83, n1);
	float amount = clamp(max(d0, d1 * 0.72) * u_wetness * (0.25 + 0.75 * u_rain), 0.0, 1.0);
	vec2 refract_uv = clamp(uv + (n0 + n1 * 0.55) * amount, vec2(0.001), vec2(0.999));
	vec3 base = textureLod(screen_texture, uv, 0.0).rgb;
	vec3 refracted = textureLod(screen_texture, refract_uv, mix(0.0, 2.2, amount)).rgb;
	vec3 colour = mix(base, refracted, amount * 0.88);
	colour += vec3(0.22, 0.28, 0.36) * amount * 0.06;
	colour += vec3(1.0, 0.96, 0.90) * u_lightning_flash * 0.22;
	COLOR = vec4(colour, 1.0);
}
