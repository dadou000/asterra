shader_type canvas_item;

uniform sampler2D u_weather : filter_linear, repeat_disable;
uniform int u_product = 0;

vec3 cloud_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	vec3 clear = vec3(0.035, 0.10, 0.19);
	vec3 mid = vec3(0.48, 0.59, 0.68);
	vec3 overcast = vec3(0.96, 0.98, 1.00);
	return x < 0.55 ? mix(clear, mid, x / 0.55) : mix(mid, overcast, (x - 0.55) / 0.45);
}

vec3 precip_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	if (x < 0.10) return mix(vec3(0.02, 0.08, 0.13), vec3(0.05, 0.42, 0.72), x / 0.10);
	if (x < 0.35) return mix(vec3(0.05, 0.42, 0.72), vec3(0.08, 0.78, 0.36), (x - 0.10) / 0.25);
	if (x < 0.62) return mix(vec3(0.08, 0.78, 0.36), vec3(0.98, 0.86, 0.16), (x - 0.35) / 0.27);
	if (x < 0.82) return mix(vec3(0.98, 0.86, 0.16), vec3(0.92, 0.18, 0.09), (x - 0.62) / 0.20);
	return mix(vec3(0.92, 0.18, 0.09), vec3(0.78, 0.10, 0.82), (x - 0.82) / 0.18);
}

vec3 storm_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	if (x < 0.25) return mix(vec3(0.035, 0.06, 0.12), vec3(0.12, 0.34, 0.72), x / 0.25);
	if (x < 0.55) return mix(vec3(0.12, 0.34, 0.72), vec3(1.0, 0.82, 0.12), (x - 0.25) / 0.30);
	if (x < 0.78) return mix(vec3(1.0, 0.82, 0.12), vec3(0.98, 0.18, 0.06), (x - 0.55) / 0.23);
	return mix(vec3(0.98, 0.18, 0.06), vec3(0.88, 0.08, 0.78), (x - 0.78) / 0.22);
}

vec3 pressure_palette(float x) {
	float d = clamp((x - 0.5) * 2.0, -1.0, 1.0);
	vec3 neutral = vec3(0.92, 0.92, 0.88);
	return d < 0.0
		? mix(neutral, vec3(0.10, 0.28, 0.88), -d)
		: mix(neutral, vec3(0.88, 0.16, 0.10), d);
}

void fragment() {
	vec3 base = texture(TEXTURE, UV).rgb;
	// WeatherNative stores longitude as [0, 2pi), while the map is displayed
	// conventionally as [-pi, +pi] with the prime meridian in the centre.
	vec2 weather_uv = vec2(fract(UV.x + 0.5), UV.y);
	vec4 w = texture(u_weather, weather_uv);

	vec3 product_color;
	float opacity = 0.84;
	if (u_product == 0) {
		// Composite: visible cloud shield + radar-like precipitation + convective cores.
		product_color = mix(base * 0.42, cloud_palette(w.r), smoothstep(0.10, 0.82, w.r) * 0.64);
		product_color = mix(product_color, precip_palette(w.b), smoothstep(0.03, 0.62, w.b) * 0.88);
		product_color = mix(product_color, storm_palette(w.g), smoothstep(0.55, 0.95, w.g) * 0.82);
		opacity = 1.0;
	} else if (u_product == 1) {
		product_color = cloud_palette(w.r);
	} else if (u_product == 2) {
		product_color = precip_palette(w.b);
	} else if (u_product == 3) {
		product_color = storm_palette(w.g);
	} else {
		product_color = pressure_palette(w.a);
	}

	vec3 col = u_product == 0 ? product_color : mix(base * 0.38, product_color, opacity);

	// 30 degree longitude / 15 degree latitude reference grid.
	float dx = min(fract(UV.x * 12.0), 1.0 - fract(UV.x * 12.0));
	float dy = min(fract(UV.y * 12.0), 1.0 - fract(UV.y * 12.0));
	float grid = max(1.0 - smoothstep(0.0, 0.008, dx), 1.0 - smoothstep(0.0, 0.008, dy));
	col = mix(col, vec3(0.82, 0.88, 0.94), grid * 0.18);
	COLOR = vec4(col, 1.0);
}
