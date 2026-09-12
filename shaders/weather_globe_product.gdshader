shader_type spatial;
render_mode unshaded, cull_back, depth_draw_opaque;

uniform sampler2D u_base : filter_linear, repeat_disable;
uniform sampler2D u_weather : filter_linear, repeat_disable;
uniform sampler2D u_diagnostics : filter_linear, repeat_disable;
uniform sampler2D u_products : filter_linear, repeat_disable;
uniform sampler2D u_wind : filter_linear, repeat_disable;
uniform int u_product = 0;
uniform float u_overlay_strength : hint_range(0.0, 1.0) = 0.90;
uniform float u_grid_strength : hint_range(0.0, 1.0) = 0.17;

varying vec3 v_object_dir;

vec3 wind_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	if (x < 0.23) return mix(vec3(0.035, 0.075, 0.30), vec3(0.025, 0.31, 0.54), x / 0.23);
	if (x < 0.48) return mix(vec3(0.025, 0.31, 0.54), vec3(0.045, 0.62, 0.43), (x - 0.23) / 0.25);
	if (x < 0.72) return mix(vec3(0.045, 0.62, 0.43), vec3(0.52, 0.88, 0.15), (x - 0.48) / 0.24);
	if (x < 0.88) return mix(vec3(0.52, 0.88, 0.15), vec3(1.0, 0.78, 0.12), (x - 0.72) / 0.16);
	return mix(vec3(1.0, 0.78, 0.12), vec3(0.95, 0.16, 0.12), (x - 0.88) / 0.12);
}

vec3 cloud_palette(float x) {
	return mix(vec3(0.025, 0.07, 0.14), vec3(0.96, 0.98, 1.0), smoothstep(0.05, 0.92, x));
}

vec3 precip_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	if (x < 0.30) return mix(vec3(0.02, 0.09, 0.20), vec3(0.04, 0.56, 0.88), x / 0.30);
	if (x < 0.60) return mix(vec3(0.04, 0.56, 0.88), vec3(0.16, 0.86, 0.34), (x - 0.30) / 0.30);
	if (x < 0.82) return mix(vec3(0.16, 0.86, 0.34), vec3(1.0, 0.82, 0.13), (x - 0.60) / 0.22);
	return mix(vec3(1.0, 0.82, 0.13), vec3(0.90, 0.06, 0.25), (x - 0.82) / 0.18);
}

vec3 storm_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	if (x < 0.38) return mix(vec3(0.025, 0.06, 0.16), vec3(0.12, 0.45, 0.88), x / 0.38);
	if (x < 0.68) return mix(vec3(0.12, 0.45, 0.88), vec3(1.0, 0.78, 0.10), (x - 0.38) / 0.30);
	return mix(vec3(1.0, 0.78, 0.10), vec3(0.95, 0.06, 0.12), (x - 0.68) / 0.32);
}

vec3 pressure_palette(float x) {
	float d = clamp((x - 0.5) * 2.0, -1.0, 1.0);
	vec3 n = vec3(0.86, 0.88, 0.90);
	return d < 0.0 ? mix(n, vec3(0.08, 0.24, 0.82), -d) : mix(n, vec3(0.90, 0.15, 0.08), d);
}

vec3 temperature_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	if (x < 0.28) return mix(vec3(0.18, 0.07, 0.55), vec3(0.08, 0.44, 0.92), x / 0.28);
	if (x < 0.55) return mix(vec3(0.08, 0.44, 0.92), vec3(0.10, 0.76, 0.49), (x - 0.28) / 0.27);
	if (x < 0.76) return mix(vec3(0.10, 0.76, 0.49), vec3(1.0, 0.80, 0.12), (x - 0.55) / 0.21);
	return mix(vec3(1.0, 0.80, 0.12), vec3(0.86, 0.05, 0.08), (x - 0.76) / 0.24);
}

vec3 cape_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	if (x < 0.35) return mix(vec3(0.025, 0.08, 0.17), vec3(0.08, 0.62, 0.42), x / 0.35);
	if (x < 0.68) return mix(vec3(0.08, 0.62, 0.42), vec3(1.0, 0.80, 0.10), (x - 0.35) / 0.33);
	return mix(vec3(1.0, 0.80, 0.10), vec3(0.92, 0.04, 0.28), (x - 0.68) / 0.32);
}

vec3 signed_palette(float x) {
	float d = clamp((x - 0.5) * 2.0, -1.0, 1.0);
	vec3 n = vec3(0.72, 0.76, 0.78);
	return d < 0.0 ? mix(n, vec3(0.06, 0.30, 0.94), -d) : mix(n, vec3(0.95, 0.10, 0.08), d);
}

vec3 shear_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	return x < 0.55 ? mix(vec3(0.025, 0.10, 0.18), vec3(0.08, 0.78, 0.48), x / 0.55)
		: mix(vec3(0.08, 0.78, 0.48), vec3(0.95, 0.12, 0.08), (x - 0.55) / 0.45);
}

vec2 globe_uv(vec3 d) {
	d = normalize(d);
	float lon = atan(d.z, d.x);
	float lat = asin(clamp(d.y, -1.0, 1.0));
	return vec2(fract(lon / (2.0 * PI) + 0.5), 0.5 - lat / PI);
}

void vertex() {
	v_object_dir = normalize(VERTEX);
}

void fragment() {
	vec2 uv = globe_uv(v_object_dir);
	vec2 field_uv = vec2(fract(uv.x + 0.5), uv.y);
	vec3 base = texture(u_base, uv).rgb;
	vec4 w = texture(u_weather, field_uv);
	vec4 d = texture(u_diagnostics, field_uv);
	vec4 p = texture(u_products, field_uv);
	vec4 wind = texture(u_wind, field_uv);
	vec3 product;

	if (u_product == 0) {
		product = wind_palette(clamp(wind.b / 65.0, 0.0, 1.0));
	} else if (u_product == 1) {
		product = mix(base * 0.52, cloud_palette(w.r), smoothstep(0.12, 0.85, w.r) * 0.55);
		product = mix(product, precip_palette(w.b), smoothstep(0.04, 0.65, w.b) * 0.86);
		product = mix(product, storm_palette(w.g), smoothstep(0.45, 0.90, w.g) * 0.78);
	} else if (u_product == 2) {
		product = cloud_palette(w.r);
	} else if (u_product == 3) {
		product = precip_palette(w.b);
	} else if (u_product == 4) {
		product = storm_palette(w.g);
	} else if (u_product == 5) {
		product = pressure_palette(w.a);
	} else if (u_product == 6) {
		product = temperature_palette(p.r);
	} else if (u_product == 7) {
		product = p.g < 0.0 ? base * 0.35 : temperature_palette(clamp((p.g * 45.0 + 40.0) / 100.0, 0.0, 1.0));
	} else if (u_product == 8) {
		product = cape_palette(p.b);
	} else if (u_product == 9) {
		product = mix(vec3(0.015, 0.03, 0.10), vec3(1.0, 0.90, 0.28), clamp(p.a, 0.0, 1.0));
	} else if (u_product == 10) {
		product = signed_palette(d.r);
	} else if (u_product == 11) {
		product = signed_palette(d.g);
	} else if (u_product == 12) {
		product = signed_palette(d.b);
	} else {
		product = shear_palette(d.a);
	}

	vec3 col = mix(base * 0.34, product, u_overlay_strength);
	if (u_product == 1) col = product;

	// Distance to the nearest meridian/parallel, in cell units: 0 on the line.
	float gx = abs(fract(uv.x * 24.0 + 0.5) - 0.5);
	float gy = abs(fract(uv.y * 12.0 + 0.5) - 0.5);
	float grid = max(1.0 - smoothstep(0.002, 0.014, gx), 1.0 - smoothstep(0.002, 0.014, gy));
	col = mix(col, vec3(0.72, 0.82, 0.92), grid * u_grid_strength);

	float limb = pow(1.0 - abs(dot(normalize(NORMAL), normalize(VIEW))), 4.0);
	// `unshaded` outputs ALBEDO only; a colour written to EMISSION never reaches
	// the framebuffer, which drew the whole globe black.
	ALBEDO = col + vec3(0.04, 0.16, 0.24) * limb;
	ROUGHNESS = 1.0;
}
