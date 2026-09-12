shader_type spatial;
render_mode unshaded, cull_disabled, depth_test_disabled, shadows_disabled, fog_disabled;

uniform sampler2D u_local_wind : filter_nearest, repeat_disable;
uniform vec3 u_center_dir = vec3(0.0, 1.0, 0.0);
uniform vec3 u_local_east = vec3(1.0, 0.0, 0.0);
uniform vec3 u_local_north = vec3(0.0, 0.0, 1.0);
uniform float u_planet_radius_m = 3500000.0;
uniform float u_local_span_m = 422400.0;
uniform float u_layer_height_m = 100.0;
uniform float u_radial_lift_m = 35.0;
uniform float u_cell_size_m = 2200.0;
uniform float u_speed_reference_mps = 55.0;
uniform float u_min_length_fraction = 0.18;
uniform float u_max_length_fraction = 0.92;
uniform float u_width_fraction = 0.055;

varying float v_speed01;
varying float v_visible;

vec3 speed_palette(float x) {
	x = clamp(x, 0.0, 1.0);
	if (x < 0.30) return mix(vec3(0.16, 0.46, 1.0), vec3(0.05, 0.86, 0.75), x / 0.30);
	if (x < 0.64) return mix(vec3(0.05, 0.86, 0.75), vec3(0.54, 1.0, 0.20), (x - 0.30) / 0.34);
	if (x < 0.84) return mix(vec3(0.54, 1.0, 0.20), vec3(1.0, 0.82, 0.12), (x - 0.64) / 0.20);
	return mix(vec3(1.0, 0.82, 0.12), vec3(1.0, 0.18, 0.08), (x - 0.84) / 0.16);
}

void vertex() {
	vec2 cell_uv = INSTANCE_CUSTOM.rg;
	vec4 wind_sample = textureLod(u_local_wind, cell_uv, 0.0);
	vec2 uv_offset = cell_uv - vec2(0.5);
	float ex = uv_offset.x * u_local_span_m;
	float ny = uv_offset.y * u_local_span_m;

	// Match the native nest geometry exactly: its x/y cells live in the fixed
	// transported east/north tangent basis and are normalized back to the sphere.
	vec3 radial = normalize(u_center_dir
		+ u_local_east * (ex / u_planet_radius_m)
		+ u_local_north * (ny / u_planet_radius_m));
	float radius = u_planet_radius_m + u_layer_height_m + u_radial_lift_m;
	vec3 anchor_world = radial * radius;
	vec3 center_world = normalize(u_center_dir) * radius;
	vec3 anchor_relative = anchor_world - center_world;

	vec3 flow = u_local_east * wind_sample.r + u_local_north * wind_sample.g;
	flow -= radial * dot(flow, radial);
	float speed = length(flow);
	vec3 fallback_along = u_local_east - radial * dot(u_local_east, radial);
	if (length(fallback_along) < 1e-5) fallback_along = u_local_north - radial * dot(u_local_north, radial);
	fallback_along = normalize(fallback_along);
	vec3 along_dir = speed > 0.01 ? flow / speed : fallback_along;
	vec3 side_dir = normalize(cross(radial, along_dir));

	float speed01 = clamp(speed / u_speed_reference_mps, 0.0, 1.0);
	// Debug geometry must remain readable even when the selected layer lies below
	// local terrain. Depth testing is disabled for this overlay, so arrows report
	// the actual model-layer vector rather than disappearing inside mountains.
	float visible = smoothstep(0.20, 1.00, speed);
	float arrow_length = u_cell_size_m
		* mix(u_min_length_fraction, u_max_length_fraction, speed01) * visible;
	float arrow_width = u_cell_size_m * u_width_fraction * visible;

	VERTEX = anchor_relative + along_dir * (VERTEX.x * arrow_length)
		+ side_dir * (VERTEX.y * arrow_width);
	v_speed01 = speed01;
	v_visible = visible;
}

void fragment() {
	vec3 color = speed_palette(v_speed01);
	ALBEDO = color;
	EMISSION = color * (0.55 + 0.45 * v_visible);
}
