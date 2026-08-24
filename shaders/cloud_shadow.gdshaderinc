/// Cloud optical-depth shadowing for opaque/water surface shaders.
///
/// This intentionally shares the same macro density model, NoiseTexture3D and
/// wind coordinate as the visible volumetric clouds. It integrates droplet
/// extinction along the actual surface -> Helion ray through Asterra's spherical
/// cloud shell, so shadows are continuous-valued Beer-Lambert transmittance rather
/// than a binary projected mask.

uniform sampler3D u_cloud_shadow_shape_noise : repeat_enable, filter_linear;
uniform float u_cloud_shadow_enabled = 0.0;
uniform float u_cloud_shadow_base = 1100.0;
uniform float u_cloud_shadow_top = 6400.0;
uniform float u_cloud_shadow_coverage = 0.52;
uniform float u_cloud_shadow_weather_variation = 0.30;
uniform float u_cloud_shadow_density = 1.0;
uniform float u_cloud_shadow_shape_scale = 0.000055;
uniform float u_cloud_shadow_weather_scale = 0.0000085;
uniform float u_cloud_shadow_extinction = 0.00105;
uniform vec3 u_cloud_shadow_wind_offset = vec3(0.0);
uniform int u_cloud_shadow_steps = 6;

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

float cloud_shadow_vertical_profile(float h, float cloud_type) {
	float bottom = smoothstep(0.0, 0.10, h);
	float top_start = mix(0.44, 0.72, cloud_type);
	float top = 1.0 - smoothstep(top_start, 1.0, h);
	return bottom * top;
}

/// Same cheap density representation used for cloud light-ray integration in the
/// sky shader: one weather sample and one body sample. Edge erosion is omitted
/// deliberately because a many-kilometre solar path naturally integrates it away.
float cloud_shadow_density_at(vec3 p, float planet_radius) {
	float altitude = length(p) - planet_radius;
	if (altitude <= u_cloud_shadow_base || altitude >= u_cloud_shadow_top) {
		return 0.0;
	}

	float h = cloud_shadow_remap01(altitude, u_cloud_shadow_base, u_cloud_shadow_top);
	vec3 surface_p = normalize(p) * (planet_radius + 100.0);
	float weather = texture(u_cloud_shadow_shape_noise,
		(surface_p + u_cloud_shadow_wind_offset * 0.16)
		* u_cloud_shadow_weather_scale).r;
	float local_coverage = clamp(
		u_cloud_shadow_coverage
			+ (weather - 0.5) * u_cloud_shadow_weather_variation,
		0.06, 0.96);
	float threshold = 1.0 - local_coverage * 0.78;
	float macro = texture(u_cloud_shadow_shape_noise,
		(p + u_cloud_shadow_wind_offset) * u_cloud_shadow_shape_scale).r;
	float body = smoothstep(threshold, min(threshold + 0.20, 0.99), macro);
	float cloud_type = smoothstep(0.22, 0.82, weather);
	return body * cloud_shadow_vertical_profile(h, cloud_type)
		* u_cloud_shadow_density;
}

/// Find the first contiguous cloud-shell segment encountered from a surface point
/// towards the star. This also works for high mountains/aircraft already inside
/// the cloud layer and for geometry viewed from above it.
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
		// Ordinary ground/water point: skip clear air below the condensation base.
		ray_start = max(ray_start, inner_hit.y);
	} else if (source_radius < outer_radius) {
		// Source is inside cloud altitude. Looking downward exits through the inner
		// boundary; looking upward traverses to cloud top.
		ray_start = 0.0;
		if (inner_hit.x > 0.0 && inner_hit.x < ray_end) {
			ray_end = inner_hit.x;
		}
	} else {
		// Source is above cloud top. Keep only the first shell segment.
		if (inner_hit.x > ray_start && inner_hit.x < ray_end) {
			ray_end = inner_hit.x;
		}
	}

	if (ray_end <= ray_start) return vec2(1e9, -1e9);
	return vec2(ray_start, ray_end);
}

float cloud_shadow_hash(vec3 p) {
	return fract(sin(dot(p, vec3(0.001731, 0.002417, 0.001193))) * 43758.5453);
}

/// Fraction of direct stellar irradiance reaching the receiver after cloud
/// absorption/out-scattering. Water droplets are effectively neutral over the
/// visible band at this scale, so one scalar transmittance is appropriate; sky
/// colour remains supplied by the separate diffuse atmosphere term.
float cloud_shadow_transmittance(vec3 receiver, vec3 sun_dir, float planet_radius) {
	if (u_cloud_shadow_enabled < 0.5) return 1.0;
	vec2 segment = cloud_shadow_layer_segment(receiver, normalize(sun_dir), planet_radius);
	if (segment.x > segment.y) return 1.0;

	int steps = clamp(u_cloud_shadow_steps, 2, CLOUD_SHADOW_MAX_STEPS);
	float path_length = segment.y - segment.x;
	float step_len = path_length / float(steps);
	float jitter = cloud_shadow_hash(receiver);
	float optical_depth = 0.0;

	for (int i = 0; i < CLOUD_SHADOW_MAX_STEPS; i++) {
		if (i >= steps) break;
		float t = segment.x + (float(i) + mix(0.32, 0.68, jitter)) * step_len;
		vec3 p = receiver + normalize(sun_dir) * t;
		optical_depth += cloud_shadow_density_at(p, planet_radius) * step_len;
		// Past ~9 optical depths the direct solar term is visually negligible;
		// stopping here saves work below very deep cloud towers.
		if (optical_depth * u_cloud_shadow_extinction > 9.0) {
			return 0.00012;
		}
	}

	return exp(-optical_depth * u_cloud_shadow_extinction);
}
