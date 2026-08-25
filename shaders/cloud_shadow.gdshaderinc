/// Cloud optical-depth shadowing driven by the same simulated global/local weather
/// fields as the depth-aware volumetric renderer.

uniform sampler3D u_cloud_shadow_shape_noise : repeat_enable, filter_linear;
uniform sampler2D u_cloud_weather_global : filter_linear;
uniform sampler2D u_cloud_weather_local : filter_linear;
uniform float u_cloud_shadow_enabled = 0.0;
uniform float u_cloud_shadow_base = 700.0;
uniform float u_cloud_shadow_top = 14500.0;
uniform float u_cloud_shadow_density = 1.15;
uniform float u_cloud_shadow_shape_scale = 0.000055;
uniform float u_cloud_shadow_extinction = 0.0010;
uniform vec3 u_cloud_shadow_wind_offset = vec3(0.0);
uniform vec3 u_cloud_shadow_sun_dir = vec3(1.0, 0.0, 0.0);
uniform int u_cloud_shadow_steps = 6;
uniform vec3 u_cloud_weather_center = vec3(0.0, 1.0, 0.0);
uniform vec3 u_cloud_weather_east = vec3(1.0, 0.0, 0.0);
uniform vec3 u_cloud_weather_north = vec3(0.0, 0.0, 1.0);
uniform float u_cloud_weather_local_span = 422400.0;

const int CLOUD_SHADOW_MAX_STEPS = 10;

vec2 cloud_shadow_sphere_hit(vec3 origin, vec3 dir, float radius) {
	float b = dot(dir, origin);
	float c = dot(origin, origin) - radius * radius;
	float disc = b * b - c;
	if (disc < 0.0) return vec2(1e9, -1e9);
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
}

float cloud_shadow_remap01(float v, float a, float b) {
	return clamp((v - a) / max(b - a, 1e-5), 0.0, 1.0);
}

vec2 cloud_shadow_global_uv(vec3 d) {
	d = normalize(d);
	float lon = atan(d.z, d.x);
	if (lon < 0.0) lon += TAU;
	float lat = asin(clamp(d.y, -1.0, 1.0));
	return vec2(lon / TAU, (PI * 0.5 - lat) / PI);
}

vec4 cloud_shadow_weather(vec3 surface_p, float planet_radius) {
	vec3 d = normalize(surface_p);
	vec4 global_state = textureLod(u_cloud_weather_global, cloud_shadow_global_uv(d), 0.0);
	vec3 center = normalize(u_cloud_weather_center);
	vec3 delta = d * planet_radius - center * planet_radius;
	vec2 local_uv = vec2(
		dot(delta, normalize(u_cloud_weather_east)),
		dot(delta, normalize(u_cloud_weather_north))) / max(u_cloud_weather_local_span, 1000.0) + vec2(0.5);
	float edge = max(abs(local_uv.x - 0.5), abs(local_uv.y - 0.5));
	float blend = 1.0 - smoothstep(0.42, 0.50, edge);
	vec4 local_state = textureLod(u_cloud_weather_local, clamp(local_uv, vec2(0.0), vec2(1.0)), 0.0);
	return mix(global_state, local_state, blend);
}

float cloud_shadow_vertical_profile(float h, float storm) {
	float bottom = smoothstep(0.0, mix(0.055, 0.025, storm), h);
	float top_start = mix(0.30, 0.84, storm);
	float top = 1.0 - smoothstep(top_start, 1.0, h);
	float anvil = smoothstep(0.58, 0.78, storm) * exp(-pow((h - 0.84) / 0.10, 2.0));
	return max(bottom * top, anvil * 0.72);
}

float cloud_shadow_density_at(vec3 p, float planet_radius) {
	float altitude = length(p) - planet_radius;
	if (altitude <= u_cloud_shadow_base || altitude >= u_cloud_shadow_top) return 0.0;
	float h = cloud_shadow_remap01(altitude, u_cloud_shadow_base, u_cloud_shadow_top);
	vec4 weather = cloud_shadow_weather(normalize(p) * planet_radius, planet_radius);
	float coverage = clamp(weather.r + (0.5 - weather.a) * 0.20 + weather.b * 0.14, 0.01, 0.99);
	float threshold = 1.0 - coverage * 0.82;
	float macro = textureLod(u_cloud_shadow_shape_noise,
		(p + u_cloud_shadow_wind_offset) * u_cloud_shadow_shape_scale, 0.0).r;
	float body = smoothstep(threshold, min(threshold + 0.18, 0.995), macro);
	return body * cloud_shadow_vertical_profile(h, weather.b)
		* u_cloud_shadow_density * (0.78 + 0.46 * weather.b + 0.22 * weather.z);
}

vec2 cloud_shadow_layer_segment(vec3 origin, vec3 dir, float planet_radius) {
	float inner_radius = planet_radius + u_cloud_shadow_base;
	float outer_radius = planet_radius + u_cloud_shadow_top;
	vec2 outer_hit = cloud_shadow_sphere_hit(origin, dir, outer_radius);
	if (outer_hit.y <= 0.0) return vec2(1e9, -1e9);
	float source_radius = length(origin);
	float ray_start = max(outer_hit.x, 0.0);
	float ray_end = outer_hit.y;
	vec2 inner_hit = cloud_shadow_sphere_hit(origin, dir, inner_radius);
	if (source_radius < inner_radius) {
		ray_start = max(ray_start, inner_hit.y);
	} else if (source_radius < outer_radius) {
		ray_start = 0.0;
		if (inner_hit.x > 0.0 && inner_hit.x < ray_end) ray_end = inner_hit.x;
	} else if (inner_hit.x > ray_start && inner_hit.x < ray_end) {
		ray_end = inner_hit.x;
	}
	if (ray_end <= ray_start) return vec2(1e9, -1e9);
	return vec2(ray_start, ray_end);
}

float cloud_shadow_hash(vec3 p) {
	return fract(sin(dot(p, vec3(0.001731, 0.002417, 0.001193))) * 43758.5453);
}

float cloud_shadow_transmittance(vec3 receiver, vec3 sun_dir, float planet_radius) {
	if (u_cloud_shadow_enabled < 0.5) return 1.0;
	vec3 s = normalize(sun_dir);
	vec2 segment = cloud_shadow_layer_segment(receiver, s, planet_radius);
	if (segment.x > segment.y) return 1.0;
	int steps = clamp(u_cloud_shadow_steps, 2, CLOUD_SHADOW_MAX_STEPS);
	float step_len = (segment.y - segment.x) / float(steps);
	float jitter = cloud_shadow_hash(receiver);
	float optical_depth = 0.0;
	for (int i = 0; i < CLOUD_SHADOW_MAX_STEPS; i++) {
		if (i >= steps) break;
		float t = segment.x + (float(i) + mix(0.32, 0.68, jitter)) * step_len;
		optical_depth += cloud_shadow_density_at(receiver + s * t, planet_radius) * step_len;
		if (optical_depth * u_cloud_shadow_extinction > 9.0) return 0.00012;
	}
	return exp(-optical_depth * u_cloud_shadow_extinction);
}
