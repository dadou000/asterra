/// Cloud optical-depth shadowing for opaque/water surface shaders.
///
/// This intentionally shares the same macro density model, NoiseTexture3D and
/// wind coordinate as the visible volumetric clouds. It integrates droplet
/// extinction through Asterra's spherical cloud shell and averages Beer-Lambert
/// transmittance over Helion's finite apparent disc. Penumbra width therefore
/// emerges from physical source angular size and cloud/receiver separation rather
/// than from a post-process blur or an arbitrary softness constant.

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
uniform vec3 u_cloud_shadow_sun_dir = vec3(1.0, 0.0, 0.0);
uniform int u_cloud_shadow_steps = 6;

/// Physical Helion geometry in metres. The apparent angular radius is calculated
/// as asin(radius / distance), so changing either value changes the penumbra
/// automatically. Defaults are a Sol-radius star at one astronomical unit.
uniform float u_cloud_shadow_helion_radius_m = 696340000.0;
uniform float u_cloud_shadow_helion_distance_m = 149597870700.0;
uniform int u_cloud_shadow_disc_samples = 5;

const int CLOUD_SHADOW_MAX_STEPS = 10;
const int CLOUD_SHADOW_MAX_DISC_SAMPLES = 7;
const float CLOUD_SHADOW_GOLDEN_ANGLE = 2.399963229728653;

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
/// visible cloud shader: one weather sample and one body sample. Explicit LOD 0
/// keeps this helper legal and deterministic in vertex shaders, where implicit
/// derivatives do not exist.
float cloud_shadow_density_at(vec3 p, float planet_radius) {
	float altitude = length(p) - planet_radius;
	if (altitude <= u_cloud_shadow_base || altitude >= u_cloud_shadow_top) {
		return 0.0;
	}

	float h = cloud_shadow_remap01(altitude, u_cloud_shadow_base, u_cloud_shadow_top);
	vec3 surface_p = normalize(p) * (planet_radius + 100.0);
	float weather = textureLod(u_cloud_shadow_shape_noise,
		(surface_p + u_cloud_shadow_wind_offset * 0.16)
		* u_cloud_shadow_weather_scale, 0.0).r;
	float local_coverage = clamp(
		u_cloud_shadow_coverage
			+ (weather - 0.5) * u_cloud_shadow_weather_variation,
		0.06, 0.96);
	float threshold = 1.0 - local_coverage * 0.78;
	float macro = textureLod(u_cloud_shadow_shape_noise,
		(p + u_cloud_shadow_wind_offset) * u_cloud_shadow_shape_scale, 0.0).r;
	float body = smoothstep(threshold, min(threshold + 0.20, 0.99), macro);
	float cloud_type = smoothstep(0.22, 0.82, weather);
	return body * cloud_shadow_vertical_profile(h, cloud_type)
		* u_cloud_shadow_density;
}

/// Find the first contiguous cloud-shell segment encountered from a surface point
/// along one ray toward a point on Helion's disc. This also works for high
/// mountains/aircraft already inside the cloud layer and geometry above it.
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
		if (inner_hit.x > 0.0 && inner_hit.x < ray_end) {
			ray_end = inner_hit.x;
		}
	} else if (inner_hit.x > ray_start && inner_hit.x < ray_end) {
		ray_end = inner_hit.x;
	}

	if (ray_end <= ray_start) return vec2(1e9, -1e9);
	return vec2(ray_start, ray_end);
}

/// Beer-Lambert transmission for one infinitesimal point on the stellar disc.
/// Midpoint integration is deliberately deterministic: hashed step jitter caused
/// temporal crawling elsewhere in the cloud renderer and is unnecessary once the
/// finite stellar disc already supplies spatial integration.
float cloud_shadow_ray_transmittance(vec3 receiver, vec3 ray_dir,
		float planet_radius) {
	vec2 segment = cloud_shadow_layer_segment(receiver, ray_dir, planet_radius);
	if (segment.x > segment.y) return 1.0;

	int steps = clamp(u_cloud_shadow_steps, 2, CLOUD_SHADOW_MAX_STEPS);
	float path_length = segment.y - segment.x;
	float step_len = path_length / float(steps);
	float optical_depth = 0.0;

	for (int i = 0; i < CLOUD_SHADOW_MAX_STEPS; i++) {
		if (i >= steps) break;
		float t = segment.x + (float(i) + 0.5) * step_len;
		vec3 p = receiver + ray_dir * t;
		optical_depth += cloud_shadow_density_at(p, planet_radius) * step_len;
		if (optical_depth * u_cloud_shadow_extinction > 9.0) {
			return 0.00012;
		}
	}

	return exp(-optical_depth * u_cloud_shadow_extinction);
}

/// Fraction of total Helion irradiance reaching the receiver after cloud
/// absorption/out-scattering. Each sample is one equal-area point on the apparent
/// stellar disc. Averaging transmittance (not optical depth) is physically correct:
/// every visible patch of the star contributes irradiance independently.
float cloud_shadow_transmittance(vec3 receiver, vec3 sun_dir, float planet_radius) {
	if (u_cloud_shadow_enabled < 0.5) return 1.0;
	vec3 s = normalize(sun_dir);

	int disc_samples = clamp(u_cloud_shadow_disc_samples,
		1, CLOUD_SHADOW_MAX_DISC_SAMPLES);
	if (disc_samples <= 1) {
		return cloud_shadow_ray_transmittance(receiver, s, planet_radius);
	}

	float radius_distance_ratio = clamp(
		u_cloud_shadow_helion_radius_m
			/ max(u_cloud_shadow_helion_distance_m, 1.0),
		0.0, 0.999999);
	float angular_radius = asin(radius_distance_ratio);
	float disc_slope = tan(angular_radius);
	if (disc_slope < 1e-7) {
		return cloud_shadow_ray_transmittance(receiver, s, planet_radius);
	}

	// Stable tangent frame around the star centre direction.
	vec3 reference_axis = abs(s.z) < 0.999
		? vec3(0.0, 0.0, 1.0)
		: vec3(0.0, 1.0, 0.0);
	vec3 tangent = normalize(cross(reference_axis, s));
	vec3 bitangent = normalize(cross(s, tangent));

	float transmittance_sum = 0.0;
	for (int j = 0; j < CLOUD_SHADOW_MAX_DISC_SAMPLES; j++) {
		if (j >= disc_samples) break;

		// Vogel / golden-angle equal-area disc pattern. The half-sample radial
		// offset avoids putting excessive weight at the exact centre for small N.
		float sample_index = float(j) + 0.5;
		float radial = sqrt(sample_index / float(disc_samples));
		float phi = sample_index * CLOUD_SHADOW_GOLDEN_ANGLE;
		vec2 disc_point = vec2(cos(phi), sin(phi)) * radial;
		vec3 ray_dir = normalize(
			s + (tangent * disc_point.x + bitangent * disc_point.y) * disc_slope);
		transmittance_sum += cloud_shadow_ray_transmittance(
			receiver, ray_dir, planet_radius);
	}

	return transmittance_sum / float(disc_samples);
}
