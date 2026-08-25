// Dynamic land-surface state published by WeatherNative.
// RGBA = normalized surface temperature, snow cover, dynamic albedo, wetness/water.
uniform sampler2D u_surface_energy_global : filter_linear, repeat_enable;
uniform sampler2D u_surface_energy_local : filter_linear, repeat_disable;
uniform float u_surface_energy_ready = 0.0;
uniform vec3 u_surface_energy_local_center = vec3(0.0, 1.0, 0.0);
uniform vec3 u_surface_energy_local_east = vec3(1.0, 0.0, 0.0);
uniform vec3 u_surface_energy_local_north = vec3(0.0, 0.0, 1.0);
uniform float u_surface_energy_local_span_m = 0.0;
uniform float u_surface_energy_planet_radius = 3500000.0;

vec2 surface_energy_global_uv(vec3 dir) {
	float lon = atan(dir.z, dir.x);
	if (lon < 0.0) lon += 6.28318530718;
	float lat = asin(clamp(dir.y, -1.0, 1.0));
	return vec2(lon / 6.28318530718, 0.5 - lat / 3.14159265359);
}

vec3 surface_energy_local_lookup(vec3 dir) {
	if (u_surface_energy_local_span_m <= 1.0) return vec3(0.0, 0.0, -1.0);
	vec3 center = normalize(u_surface_energy_local_center);
	float forward = dot(dir, center);
	if (forward <= 0.0) return vec3(0.0, 0.0, -1.0);
	float east_angle = atan(dot(dir, normalize(u_surface_energy_local_east)), forward);
	float north_angle = atan(dot(dir, normalize(u_surface_energy_local_north)), forward);
	vec2 local_uv = vec2(east_angle, north_angle)
		* (u_surface_energy_planet_radius / u_surface_energy_local_span_m) + vec2(0.5);
	vec2 edge_v = abs(local_uv - vec2(0.5)) * 2.0;
	float edge = max(edge_v.x, edge_v.y);
	if (edge >= 1.0) return vec3(local_uv, -1.0);
	return vec3(local_uv, 1.0 - smoothstep(0.86, 1.0, edge));
}

vec4 surface_energy_sample(vec3 planet_position) {
	if (u_surface_energy_ready < 0.5 || dot(planet_position, planet_position) < 1.0) {
		return vec4((288.0 - 220.0) / 110.0, 0.0, 0.18, 0.35);
	}
	vec3 dir = normalize(planet_position);
	vec4 coarse = texture(u_surface_energy_global, surface_energy_global_uv(dir));
	vec3 lookup = surface_energy_local_lookup(dir);
	if (lookup.z < 0.0) return coarse;
	return mix(coarse, texture(u_surface_energy_local, lookup.xy), lookup.z);
}
