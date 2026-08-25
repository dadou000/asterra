shader_type canvas_item;

uniform sampler2D u_weather : filter_linear, repeat_disable;
uniform sampler2D u_local_weather : filter_linear, repeat_disable;
uniform sampler2D u_diagnostics : filter_linear, repeat_disable;
uniform sampler2D u_local_diagnostics : filter_linear, repeat_disable;
uniform sampler2D u_products : filter_linear, repeat_disable;
uniform sampler2D u_local_products : filter_linear, repeat_disable;
uniform vec3 u_local_center = vec3(0.0, 1.0, 0.0);
uniform vec3 u_local_east = vec3(1.0, 0.0, 0.0);
uniform vec3 u_local_north = vec3(0.0, 0.0, 1.0);
uniform float u_local_span_m = 0.0;
uniform float u_planet_radius = 1000000.0;
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
	return d < 0.0 ? mix(neutral, vec3(0.10, 0.28, 0.88), -d)
		: mix(neutral, vec3(0.88, 0.16, 0.10), d);
}

vec3 signed_dynamic_palette(float x) {
	float d = clamp((x - 0.5) * 2.0, -1.0, 1.0);
	vec3 neutral = vec3(0.88, 0.90, 0.90);
	return d < 0.0 ? mix(neutral, vec3(0.10, 0.36, 0.92), -d)
		: mix(neutral, vec3(0.92, 0.16, 0.12), d);
}

vec3 pv_palette(float x) {
	float d = clamp((x - 0.5) * 2.0, -1.0, 1.0);
	vec3 neutral = vec3(0.90, 0.90, 0.86);
	return d < 0.0 ? mix(neutral, vec3(0.20, 0.20, 0.70), -d)
		: mix(neutral, vec3(0.72, 0.08, 0.72), d);
}

vec3 shear_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	if (x < 0.35) return mix(vec3(0.03, 0.10, 0.16), vec3(0.10, 0.62, 0.54), x / 0.35);
	if (x < 0.68) return mix(vec3(0.10, 0.62, 0.54), vec3(0.96, 0.82, 0.14), (x - 0.35) / 0.33);
	return mix(vec3(0.96, 0.82, 0.14), vec3(0.90, 0.10, 0.12), (x - 0.68) / 0.32);
}

vec3 temperature_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	if (x < 0.25) return mix(vec3(0.22, 0.08, 0.55), vec3(0.12, 0.48, 0.92), x / 0.25);
	if (x < 0.50) return mix(vec3(0.12, 0.48, 0.92), vec3(0.18, 0.82, 0.62), (x - 0.25) / 0.25);
	if (x < 0.72) return mix(vec3(0.18, 0.82, 0.62), vec3(1.0, 0.82, 0.16), (x - 0.50) / 0.22);
	return mix(vec3(1.0, 0.82, 0.16), vec3(0.82, 0.06, 0.08), (x - 0.72) / 0.28);
}

vec3 cape_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	if (x < 0.25) return mix(vec3(0.04, 0.11, 0.20), vec3(0.12, 0.62, 0.48), x / 0.25);
	if (x < 0.60) return mix(vec3(0.12, 0.62, 0.48), vec3(0.98, 0.80, 0.12), (x - 0.25) / 0.35);
	return mix(vec3(0.98, 0.80, 0.12), vec3(0.88, 0.04, 0.30), (x - 0.60) / 0.40);
}

vec3 irradiance_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	return x < 0.45
		? mix(vec3(0.015, 0.025, 0.09), vec3(0.18, 0.42, 0.82), x / 0.45)
		: mix(vec3(0.18, 0.42, 0.82), vec3(1.0, 0.90, 0.30), (x - 0.45) / 0.55);
}

// x/y = local texture coordinate, z = blend weight. Negative z means global only.
vec3 local_lookup(vec2 uv) {
	if (u_local_span_m <= 1.0) return vec3(0.0, 0.0, -1.0);
	float lon = (uv.x - 0.5) * TAU;
	float lat = (0.5 - uv.y) * PI;
	float cl = cos(lat);
	vec3 dir = vec3(cl * cos(lon), sin(lat), cl * sin(lon));
	vec3 center = normalize(u_local_center);
	float forward = dot(dir, center);
	if (forward <= 0.0) return vec3(0.0, 0.0, -1.0);
	float east_angle = atan(dot(dir, normalize(u_local_east)), forward);
	float north_angle = atan(dot(dir, normalize(u_local_north)), forward);
	vec2 local_uv = vec2(east_angle, north_angle) * (u_planet_radius / u_local_span_m) + vec2(0.5);
	vec2 edge_v = abs(local_uv - vec2(0.5)) * 2.0;
	float edge = max(edge_v.x, edge_v.y);
	if (edge >= 1.0) return vec3(local_uv, -1.0);
	return vec3(local_uv, 1.0 - smoothstep(0.86, 1.0, edge));
}

vec4 sample_weather_field(vec2 uv, vec3 lookup) {
	vec2 global_uv = vec2(fract(uv.x + 0.5), uv.y);
	vec4 coarse = texture(u_weather, global_uv);
	// Every longitude converges to one physical point at a pole. Average the
	// last few display rows zonally so equirectangular interpolation cannot
	// expose harmless longitude-to-longitude cell noise as a striped cap.
	float pole_distance = min(uv.y, 1.0 - uv.y);
	float pole_blend = 1.0 - smoothstep(0.0, 0.025, pole_distance);
	if (pole_blend > 0.0) {
		vec4 pole_mean = (
			texture(u_weather, vec2(0.125, global_uv.y))
			+ texture(u_weather, vec2(0.375, global_uv.y))
			+ texture(u_weather, vec2(0.625, global_uv.y))
			+ texture(u_weather, vec2(0.875, global_uv.y))) * 0.25;
		coarse = mix(coarse, pole_mean, pole_blend);
	}
	if (lookup.z < 0.0) return coarse;
	return mix(coarse, texture(u_local_weather, lookup.xy), lookup.z);
}

vec4 sample_diagnostics_field(vec2 uv, vec3 lookup) {
	vec2 global_uv = vec2(fract(uv.x + 0.5), uv.y);
	vec4 coarse = texture(u_diagnostics, global_uv);
	float pole_distance = min(uv.y, 1.0 - uv.y);
	float pole_blend = 1.0 - smoothstep(0.0, 0.025, pole_distance);
	if (pole_blend > 0.0) {
		vec4 pole_mean = (
			texture(u_diagnostics, vec2(0.125, global_uv.y))
			+ texture(u_diagnostics, vec2(0.375, global_uv.y))
			+ texture(u_diagnostics, vec2(0.625, global_uv.y))
			+ texture(u_diagnostics, vec2(0.875, global_uv.y))) * 0.25;
		coarse = mix(coarse, pole_mean, pole_blend);
	}
	if (lookup.z < 0.0) return coarse;
	return mix(coarse, texture(u_local_diagnostics, lookup.xy), lookup.z);
}

vec4 sample_products_field(vec2 uv, vec3 lookup) {
	vec2 global_uv = vec2(fract(uv.x + 0.5), uv.y);
	vec4 coarse = texture(u_products, global_uv);
	if (lookup.z < 0.0) return coarse;
	return mix(coarse, texture(u_local_products, lookup.xy), lookup.z);
}

void fragment() {
	vec3 base = texture(TEXTURE, UV).rgb;
	vec3 lookup = local_lookup(UV);
	vec4 w = sample_weather_field(UV, lookup);
	vec4 d = sample_diagnostics_field(UV, lookup);
	vec4 p = sample_products_field(UV, lookup);

	vec3 product_color;
	float opacity = 0.84;
	if (u_product == 0) {
		product_color = mix(base * 0.42, cloud_palette(w.r), smoothstep(0.10, 0.82, w.r) * 0.64);
		product_color = mix(product_color, precip_palette(w.b), smoothstep(0.03, 0.62, w.b) * 0.88);
		product_color = mix(product_color, storm_palette(w.g), smoothstep(0.55, 0.95, w.g) * 0.82);
		opacity = 1.0;
	} else if (u_product == 1) {
		product_color = mix(base * 0.20, pressure_palette(w.a), 0.78);
		product_color = mix(product_color, storm_palette(w.g), smoothstep(0.08, 0.24, w.g) * 0.62);
		opacity = 1.0;
	} else if (u_product == 2) {
		product_color = cloud_palette(w.r);
	} else if (u_product == 3) {
		product_color = precip_palette(w.b);
	} else if (u_product == 4) {
		product_color = storm_palette(w.g);
	} else if (u_product == 5) {
		product_color = pressure_palette(w.a);
	} else if (u_product == 6) {
		product_color = temperature_palette(p.r);
	} else if (u_product == 7) {
		product_color = p.g < 0.0 ? base * 0.28 : temperature_palette(clamp((p.g * 45.0 + 40.0) / 100.0, 0.0, 1.0));
	} else if (u_product == 8) {
		product_color = cape_palette(p.b);
	} else if (u_product == 9) {
		product_color = irradiance_palette(p.a);
	} else if (u_product == 10) {
		product_color = signed_dynamic_palette(d.r);
	} else if (u_product == 11) {
		product_color = signed_dynamic_palette(d.g);
	} else if (u_product == 12) {
		product_color = pv_palette(d.b);
	} else {
		product_color = shear_palette(d.a);
	}

	vec3 col = (u_product == 0 || u_product == 1) ? product_color : mix(base * 0.34, product_color, opacity);
	// The synoptic product is a meteorological analysis, so remove the final
	// terrain/storm tint at the coordinate singularity and show the zonal
	// pressure value alone through the tiny polar cap.
	if (u_product == 1) {
		float pole_blend = 1.0 - smoothstep(0.0, 0.025, min(UV.y, 1.0 - UV.y));
		col = mix(col, pressure_palette(w.a), pole_blend);
	}
	float dx = min(fract(UV.x * 12.0), 1.0 - fract(UV.x * 12.0));
	float dy = min(fract(UV.y * 12.0), 1.0 - fract(UV.y * 12.0));
	float grid = max(1.0 - smoothstep(0.0, 0.008, dx), 1.0 - smoothstep(0.0, 0.008, dy));
	col = mix(col, vec3(0.82, 0.88, 0.94), grid * 0.18);
	COLOR = vec4(col, 1.0);
}
