// 0.1.0 dynamic hydrology -> visible water contract.
//
// The texture is owned by WaterSurfaceResources on the global RenderingDevice.
// It is intentionally a local tangent-plane cache, not persistent world state.
// Production sparse hydrology will repopulate/recenter this representation.
//
// RGBA channels:
//   R = dynamic surface-height residual [m]
//   G = tangent velocity X [m/s]
//   B = tangent velocity Y [m/s]
//   A = activity/foam hint [0..1]

uniform sampler2D u_dynamic_surface_field : filter_linear, repeat_disable;
uniform float u_dynamic_surface_enabled = 0.0;
uniform vec2 u_dynamic_surface_center_plane = vec2(0.0);
uniform float u_dynamic_surface_half_extent_m = 2048.0;

struct WaterDynamicSurfaceSample {
	float height;
	vec2 velocity;
	float activity;
	float valid;
};

WaterDynamicSurfaceSample water_dynamic_surface_empty() {
	WaterDynamicSurfaceSample sample_value;
	sample_value.height = 0.0;
	sample_value.velocity = vec2(0.0);
	sample_value.activity = 0.0;
	sample_value.valid = 0.0;
	return sample_value;
}

WaterDynamicSurfaceSample water_dynamic_surface_sample(vec2 plane_m) {
	WaterDynamicSurfaceSample sample_value = water_dynamic_surface_empty();
	if (u_dynamic_surface_enabled < 0.5) return sample_value;

	float half_extent = max(u_dynamic_surface_half_extent_m, 1.0);
	vec2 local_m = plane_m - u_dynamic_surface_center_plane;
	vec2 uv = local_m / (2.0 * half_extent) + vec2(0.5);
	vec2 inside = step(vec2(0.0), uv) * step(uv, vec2(1.0));
	if (inside.x * inside.y < 0.5) return sample_value;

	vec4 packed = texture(u_dynamic_surface_field, uv);
	sample_value.height = packed.r;
	sample_value.velocity = packed.gb;
	sample_value.activity = clamp(packed.a, 0.0, 1.0);
	sample_value.valid = 1.0;
	return sample_value;
}

// Central difference in the same tangent-plane metric used by the ocean clipmap.
// The dominant hydrology normal therefore comes from the exact same reconstructed
// height field as vertex displacement instead of from an unrelated normal texture.
vec2 water_dynamic_surface_gradient(vec2 plane_m) {
	if (u_dynamic_surface_enabled < 0.5) return vec2(0.0);
	ivec2 tex_size_i = textureSize(u_dynamic_surface_field, 0);
	vec2 tex_size = max(vec2(tex_size_i), vec2(1.0));
	vec2 texel_m = vec2(2.0 * max(u_dynamic_surface_half_extent_m, 1.0)) / tex_size;

	WaterDynamicSurfaceSample c = water_dynamic_surface_sample(plane_m);
	if (c.valid < 0.5) return vec2(0.0);
	WaterDynamicSurfaceSample xm = water_dynamic_surface_sample(plane_m - vec2(texel_m.x, 0.0));
	WaterDynamicSurfaceSample xp = water_dynamic_surface_sample(plane_m + vec2(texel_m.x, 0.0));
	WaterDynamicSurfaceSample ym = water_dynamic_surface_sample(plane_m - vec2(0.0, texel_m.y));
	WaterDynamicSurfaceSample yp = water_dynamic_surface_sample(plane_m + vec2(0.0, texel_m.y));

	float hx0 = xm.valid > 0.5 ? xm.height : c.height;
	float hx1 = xp.valid > 0.5 ? xp.height : c.height;
	float hy0 = ym.valid > 0.5 ? ym.height : c.height;
	float hy1 = yp.valid > 0.5 ? yp.height : c.height;
	return vec2(
		(hx1 - hx0) / max(2.0 * texel_m.x, 1e-4),
		(hy1 - hy0) / max(2.0 * texel_m.y, 1e-4));
}
