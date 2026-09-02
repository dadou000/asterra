shader_type spatial;
render_mode cull_disabled, depth_prepass_alpha, diffuse_burley, specular_schlick_ggx;

#include "res://shaders/water_surface.gdshaderinc"

// Local inland/flood coat. It consumes the same disposable dense cache as the
// ocean coat but uses physical depth (cache A) as its wet mask, so lakes/floods
// can appear above land without making the ocean clipmap render through terrain.

uniform vec3 u_origin = vec3(0.0);
uniform vec3 u_anchor_render = vec3(0.0);
uniform float u_planet_radius = 1.0;
uniform float u_time_s = 0.0;
uniform float u_render_enabled = 0.0;
uniform float u_min_depth_m = 0.015;
uniform float u_surface_bias_m = 0.012;
uniform float u_edge_feather_m = 0.10;
uniform float u_ripple_strength = 0.035;
uniform float u_ripple_scale_m = 3.2;

uniform vec3 u_shallow : source_color = vec3(0.075, 0.31, 0.31);
uniform vec3 u_shelf : source_color = vec3(0.025, 0.145, 0.225);
uniform vec3 u_deep : source_color = vec3(0.006, 0.035, 0.090);
uniform vec3 u_foam : source_color = vec3(0.90, 0.94, 0.96);

varying vec2 v_field_plane_m;
varying vec3 v_surface_dir;
varying vec3 v_planet;
varying float v_vertex_depth;

vec3 local_water_direction(vec2 field_plane_m) {
	vec3 a = normalize(u_dynamic_surface_anchor_dir);
	vec2 offset_m = field_plane_m;
	float arc_m = length(offset_m);
	if (arc_m <= 1e-7) return a;
	float theta = arc_m / max(u_planet_radius, 1.0);
	vec3 tangent = normalize(
		u_dynamic_surface_anchor_right * offset_m.x
		+ u_dynamic_surface_anchor_up * offset_m.y);
	return normalize(a * cos(theta) + tangent * sin(theta));
}

vec3 local_water_render_offset(vec2 field_plane_m, float altitude_m) {
	vec3 center = normalize(u_dynamic_surface_anchor_dir);
	vec3 right = normalize(u_dynamic_surface_anchor_right);
	vec3 up = normalize(u_dynamic_surface_anchor_up);
	float radius = max(u_planet_radius, 1.0);
	float arc_m = length(field_plane_m);
	if (arc_m <= 1e-7) return center * altitude_m;
	float theta = arc_m / radius;
	vec3 tangent = normalize(right * field_plane_m.x + up * field_plane_m.y);
	// Relative to the radius-level anchor point. This avoids constructing the full
	// planet position in float32 and remains stable under floating-origin shifts.
	float radial_m = radius * (cos(theta) - 1.0) + altitude_m * cos(theta);
	float tangent_m = (radius + altitude_m) * sin(theta);
	return center * radial_m + tangent * tangent_m;
}

vec4 local_water_raw(vec2 field_plane_m) {
	float half_extent = max(u_dynamic_surface_half_extent_m, 1.0);
	vec2 uv = (field_plane_m - u_dynamic_surface_center_plane)
		/ (2.0 * half_extent) + vec2(0.5);
	vec2 inside = step(vec2(0.0), uv) * step(uv, vec2(1.0));
	if (inside.x * inside.y < 0.5) return vec4(0.0);
	return texture(u_dynamic_surface_field, uv);
}

// A dry vertex adjacent to a wet texel is lifted to the neighboring water level.
// The fragment wet mask still cuts the exact shoreline, but this prevents a wet
// triangle from ramping down toward datum zero merely because one corner is dry.
float local_water_vertex_eta(vec2 field_plane_m, out float depth) {
	vec4 c = local_water_raw(field_plane_m);
	depth = max(c.a, 0.0);
	if (depth > u_min_depth_m) return c.r;

	ivec2 ts = textureSize(u_dynamic_surface_field, 0);
	vec2 texel_m = 2.0 * max(u_dynamic_surface_half_extent_m, 1.0)
		/ max(vec2(ts), vec2(1.0));
	float best_depth = depth;
	float best_eta = c.r;
	vec4 sx0 = local_water_raw(field_plane_m - vec2(texel_m.x, 0.0));
	vec4 sx1 = local_water_raw(field_plane_m + vec2(texel_m.x, 0.0));
	vec4 sy0 = local_water_raw(field_plane_m - vec2(0.0, texel_m.y));
	vec4 sy1 = local_water_raw(field_plane_m + vec2(0.0, texel_m.y));
	if (sx0.a > best_depth) { best_depth = sx0.a; best_eta = sx0.r; }
	if (sx1.a > best_depth) { best_depth = sx1.a; best_eta = sx1.r; }
	if (sy0.a > best_depth) { best_depth = sy0.a; best_eta = sy0.r; }
	if (sy1.a > best_depth) { best_depth = sy1.a; best_eta = sy1.r; }
	return best_depth > u_min_depth_m ? best_eta : 0.0;
}

vec2 local_water_height_gradient(vec2 field_plane_m, float center_height,
		float center_depth) {
	ivec2 ts = textureSize(u_dynamic_surface_field, 0);
	vec2 texel_m = 2.0 * max(u_dynamic_surface_half_extent_m, 1.0)
		/ max(vec2(ts), vec2(1.0));
	vec4 xm = local_water_raw(field_plane_m - vec2(texel_m.x, 0.0));
	vec4 xp = local_water_raw(field_plane_m + vec2(texel_m.x, 0.0));
	vec4 ym = local_water_raw(field_plane_m - vec2(0.0, texel_m.y));
	vec4 yp = local_water_raw(field_plane_m + vec2(0.0, texel_m.y));
	float hx0 = xm.a > u_min_depth_m ? xm.r : center_height;
	float hx1 = xp.a > u_min_depth_m ? xp.r : center_height;
	float hy0 = ym.a > u_min_depth_m ? ym.r : center_height;
	float hy1 = yp.a > u_min_depth_m ? yp.r : center_height;
	return vec2(
		(hx1 - hx0) / max(2.0 * texel_m.x, 1e-4),
		(hy1 - hy0) / max(2.0 * texel_m.y, 1e-4));
}

vec2 local_water_ripple_gradient(vec2 p, vec2 flow, float wind_speed) {
	float scale = max(u_ripple_scale_m, 0.25);
	vec2 flow_dir = length(flow) > 0.05 ? normalize(flow) : vec2(0.894, 0.447);
	vec2 cross_dir = vec2(-flow_dir.y, flow_dir.x);
	float t = u_time_s;
	float k0 = 6.2831853 / scale;
	float k1 = 6.2831853 / (scale * 0.47);
	float a = u_ripple_strength * mix(0.35, 1.0, smoothstep(0.0, 12.0, wind_speed));
	float ph0 = dot(p, flow_dir) * k0 - t * (1.0 + length(flow) * 0.35);
	float ph1 = dot(p, normalize(flow_dir * 0.55 + cross_dir * 0.84)) * k1 - t * 1.65;
	return flow_dir * (a * cos(ph0))
		+ normalize(flow_dir * 0.55 + cross_dir * 0.84) * (a * 0.55 * cos(ph1));
}

void vertex() {
	vec2 local01 = UV;
	vec2 field_plane_m = u_dynamic_surface_center_plane
		+ (local01 * 2.0 - vec2(1.0)) * u_dynamic_surface_half_extent_m;
	float vertex_depth;
	float eta = local_water_vertex_eta(field_plane_m, vertex_depth);
	vec3 d = local_water_direction(field_plane_m);

	v_field_plane_m = field_plane_m;
	v_surface_dir = d;
	v_vertex_depth = vertex_depth;
	v_planet = d * (u_planet_radius + eta + u_surface_bias_m);
	VERTEX = u_anchor_render
		+ local_water_render_offset(field_plane_m, eta + u_surface_bias_m);
}

void fragment() {
	if (u_render_enabled < 0.5) discard;
	WaterDynamicSurfaceSample water = water_dynamic_surface_sample_field(v_field_plane_m);
	float depth = water.depth;
	if (depth <= u_min_depth_m) discard;

	float wet_alpha = smoothstep(u_min_depth_m,
		max(u_min_depth_m + u_edge_feather_m, u_min_depth_m * 2.0), depth);
	if (wet_alpha <= 0.001) discard;

	vec3 up = normalize(v_surface_dir);
	vec3 tx = u_dynamic_surface_anchor_right
		- up * dot(u_dynamic_surface_anchor_right, up);
	float tx2 = dot(tx, tx);
	if (tx2 < 1e-8) tx = normalize(cross(vec3(0.0, 1.0, 0.0), up));
	else tx *= inversesqrt(tx2);
	vec3 ty = normalize(cross(up, tx));
	if (dot(ty, u_dynamic_surface_anchor_up) < 0.0) ty = -ty;

	vec2 physical_grad = local_water_height_gradient(
		v_field_plane_m, water.height, depth);
	float wind_speed = water_wind_speed(up);
	vec2 ripple_grad = local_water_ripple_gradient(
		v_field_plane_m, water.velocity, wind_speed);
	vec2 slope = physical_grad + ripple_grad;
	vec3 n = normalize(up - tx * slope.x - ty * slope.y);
	if (dot(n, up) < 0.0) n = -n;

	vec3 camera_planet = CAMERA_POSITION_WORLD + u_origin;
	vec3 view_dir = normalize(camera_planet - v_planet);
	float fres = water_fresnel(max(dot(n, view_dir), 0.0));
	vec3 base = water_depth_tint(depth, u_shallow, u_shelf, u_deep);

	float speed = length(water.velocity);
	float flow_foam = smoothstep(1.3, 4.5, speed)
		* smoothstep(0.03, 0.25, length(physical_grad));
	float edge_foam = (1.0 - smoothstep(0.03, 0.30, depth))
		* smoothstep(0.2, 1.5, speed);
	float foam = clamp(max(flow_foam, edge_foam), 0.0, 1.0);
	base = mix(base, u_foam, foam * 0.78);

	float rough = water_roughness_from_wind(wind_speed, 0.5);
	rough = clamp(rough + foam * 0.18, 0.06, 0.72);

	ALBEDO = base;
	NORMAL = normalize((VIEW_MATRIX * vec4(n, 0.0)).xyz);
	ROUGHNESS = rough;
	SPECULAR = WATER_GODOT_SPECULAR;
	METALLIC = 0.0;
	AO = 1.0;
	// Thin flood water remains transparent enough to show terrain; deep local water
	// becomes substantially opaque while Fresnel raises grazing reflectance.
	float optical = 1.0 - exp(-depth * 0.32);
	ALPHA = clamp((0.18 + optical * 0.62 + fres * 0.20) * wet_alpha,
		0.02, 0.96);
}
