/// Planet-scale volumetric cloud model used by the atmospheric sky.
///
/// Clouds live in a spherical shell so their apparent altitude, horizon curvature,
/// and orbital silhouette remain correct everywhere on Asterra. Expensive density
/// integration runs from the sky shader's half-resolution subpass. The RGB result
/// is in the same linear-radiance domain as the atmosphere and A stores remaining
/// line-of-sight transmittance for physically ordered compositing.

uniform sampler3D u_cloud_shape_noise : repeat_enable, filter_linear;
uniform sampler3D u_cloud_detail_noise : repeat_enable, filter_linear;

uniform float u_cloud_enabled = 1.0;
uniform float u_cloud_base = 1100.0;
uniform float u_cloud_top = 6400.0;
uniform float u_cloud_coverage = 0.52;
uniform float u_cloud_weather_variation = 0.30;
uniform float u_cloud_density = 1.0;
uniform float u_cloud_shape_scale = 0.000055;
uniform float u_cloud_detail_scale = 0.00042;
uniform float u_cloud_weather_scale = 0.0000085;
uniform float u_cloud_detail_strength = 0.36;
uniform float u_cloud_extinction = 0.00105;
uniform vec3 u_cloud_wind_offset = vec3(0.0);
uniform int u_cloud_primary_steps = 40;
uniform int u_cloud_light_steps = 4;

const int CLOUD_MAX_PRIMARY_STEPS = 64;
const int CLOUD_MAX_LIGHT_STEPS = 8;

vec2 cloud_sphere_intersect(vec3 origin, vec3 dir, float radius) {
	float b = dot(dir, origin);
	float c = dot(origin, origin) - radius * radius;
	float disc = b * b - c;
	if (disc < 0.0) return vec2(1e9, -1e9);
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
}

/// Return the single visible segment through the cloud shell. A ground hit clips
/// the segment before the far side of the planet, which also prevents clouds from
/// appearing through Asterra when the camera is below the layer.
vec2 cloud_layer_segment(vec3 origin, vec3 dir) {
	float inner_radius = u_planet_radius + u_cloud_base;
	float outer_radius = u_planet_radius + u_cloud_top;
	vec2 outer_hit = cloud_sphere_intersect(origin, dir, outer_radius);
	if (outer_hit.y <= 0.0) return vec2(1e9, -1e9);

	float camera_radius = length(origin);
	float ray_start = max(outer_hit.x, 0.0);
	float ray_end = outer_hit.y;
	vec2 inner_hit = cloud_sphere_intersect(origin, dir, inner_radius);

	if (camera_radius < inner_radius) {
		// Surface / low-altitude camera: the ray first exits the clear-air sphere.
		ray_start = max(ray_start, inner_hit.y);
	} else if (camera_radius < outer_radius) {
		// Camera is inside the layer. Looking down reaches the inner boundary first;
		// looking up has a negative inner intersection and reaches the outer shell.
		ray_start = 0.0;
		if (inner_hit.x > 0.0 && inner_hit.x < ray_end) {
			ray_end = inner_hit.x;
		}
	} else {
		// Orbital camera. Stop at the inner boundary only if the ray actually crosses
		// it; grazing limb rays remain entirely inside the cloud shell.
		if (inner_hit.x > ray_start && inner_hit.x < ray_end) {
			ray_end = inner_hit.x;
		}
	}

	vec2 ground_hit = cloud_sphere_intersect(origin, dir, u_planet_radius);
	if (ground_hit.x > 0.0) {
		ray_end = min(ray_end, ground_hit.x);
	}
	if (ray_end <= ray_start) return vec2(1e9, -1e9);
	return vec2(ray_start, ray_end);
}

float cloud_remap01(float v, float a, float b) {
	return clamp((v - a) / max(b - a, 1e-5), 0.0, 1.0);
}

float cloud_vertical_profile(float h, float cloud_type) {
	// A crisp condensation base and a progressively softer, type-dependent top.
	// Low weather values produce flatter stratocumulus, high values taller cumulus.
	float bottom = smoothstep(0.0, 0.10, h);
	float top_start = mix(0.44, 0.72, cloud_type);
	float top = 1.0 - smoothstep(top_start, 1.0, h);
	return bottom * top;
}

vec3 cloud_surface_anchor(vec3 p) {
	return normalize(p) * (u_planet_radius + 100.0);
}

float cloud_weather(vec3 surface_p) {
	vec3 weather_wind = u_cloud_wind_offset * 0.16;
	float a = texture(u_cloud_shape_noise,
		(surface_p + weather_wind) * u_cloud_weather_scale).r;
	float b = texture(u_cloud_shape_noise,
		(surface_p + weather_wind * 0.73) * (u_cloud_weather_scale * 0.53)
		+ vec3(0.37, 0.11, 0.71)).r;
	return mix(a, b, 0.32);
}

/// Coarse density is deliberately only two 3D fetches. Sun transmittance calls it
/// several times per visible cloud sample, so keeping this path cheap matters more
/// than putting erosion detail into a shadow that is already integrated spatially.
float cloud_density_coarse(vec3 p) {
	float altitude = length(p) - u_planet_radius;
	if (altitude <= u_cloud_base || altitude >= u_cloud_top) return 0.0;

	float h = cloud_remap01(altitude, u_cloud_base, u_cloud_top);
	vec3 surface_p = cloud_surface_anchor(p);
	float weather = texture(u_cloud_shape_noise,
		(surface_p + u_cloud_wind_offset * 0.16) * u_cloud_weather_scale).r;
	float local_coverage = clamp(
		u_cloud_coverage + (weather - 0.5) * u_cloud_weather_variation,
		0.06, 0.96);
	float threshold = 1.0 - local_coverage * 0.78;
	float macro = texture(u_cloud_shape_noise,
		(p + u_cloud_wind_offset) * u_cloud_shape_scale).r;
	float body = smoothstep(threshold, min(threshold + 0.20, 0.99), macro);
	float cloud_type = smoothstep(0.22, 0.82, weather);
	return body * cloud_vertical_profile(h, cloud_type) * u_cloud_density;
}

float cloud_density_at(vec3 p, float detail_weight) {
	float altitude = length(p) - u_planet_radius;
	if (altitude <= u_cloud_base || altitude >= u_cloud_top) return 0.0;

	float h = cloud_remap01(altitude, u_cloud_base, u_cloud_top);
	vec3 surface_p = cloud_surface_anchor(p);
	float weather = cloud_weather(surface_p);
	float local_coverage = clamp(
		u_cloud_coverage + (weather - 0.5) * u_cloud_weather_variation,
		0.06, 0.96);
	float threshold = 1.0 - local_coverage * 0.78;

	vec3 shape_p = (p + u_cloud_wind_offset) * u_cloud_shape_scale;
	float n0 = texture(u_cloud_shape_noise, shape_p).r;
	float n1 = texture(u_cloud_shape_noise,
		shape_p * 2.03 + vec3(0.19, 0.61, 0.43)).r;
	float macro = mix(n0, n1, 0.24);
	float body = smoothstep(threshold, min(threshold + 0.20, 0.99), macro);

	float type_noise = texture(u_cloud_shape_noise,
		(surface_p + u_cloud_wind_offset * 0.11) * (u_cloud_weather_scale * 0.67)
		+ vec3(0.73, 0.29, 0.13)).r;
	float profile = cloud_vertical_profile(h, smoothstep(0.18, 0.86, type_noise));
	body *= profile;

	// High-frequency ridged noise erodes cloud edges instead of modulating the
	// whole body. Dense interiors therefore remain optically solid while wisps and
	// cauliflower detail live where the density field crosses its condensation edge.
	if (detail_weight > 0.001 && body > 0.015) {
		vec3 detail_p = (p + u_cloud_wind_offset * 1.31) * u_cloud_detail_scale;
		float detail = texture(u_cloud_detail_noise,
			detail_p + vec3(0.41, 0.17, 0.83)).r;
		float edge = 1.0 - smoothstep(0.30, 0.86, body);
		body = max(body - (1.0 - detail) * u_cloud_detail_strength
			* detail_weight * (0.22 + 0.78 * edge), 0.0);
	}

	return smoothstep(0.015, 0.30, body) * u_cloud_density;
}

float cloud_hg(float mu, float g) {
	float gg = g * g;
	return (1.0 - gg) / (4.0 * PI
		* pow(max(1.0 + gg - 2.0 * g * mu, 1e-4), 1.5));
}

float cloud_phase(float mu) {
	// Forward Mie lobe gives the silver lining; a small backscatter lobe keeps the
	// sun-facing side from becoming unnaturally flat.
	return cloud_hg(mu, 0.65) * 0.86 + cloud_hg(mu, -0.24) * 0.14;
}

float cloud_sun_transmittance(vec3 p, vec3 sun_dir, int requested_steps) {
	int light_steps = clamp(requested_steps, 1, CLOUD_MAX_LIGHT_STEPS);
	float outer_radius = u_planet_radius + u_cloud_top;
	vec2 hit = cloud_sphere_intersect(p, sun_dir, outer_radius);
	float max_distance = min(max(hit.y, 0.0), 60000.0);
	if (max_distance <= 1.0) return 1.0;
	float step_len = max_distance / float(light_steps);
	float optical_depth = 0.0;

	for (int j = 0; j < CLOUD_MAX_LIGHT_STEPS; j++) {
		if (j >= light_steps) break;
		// Bias samples toward the illuminated point. Nearby density produces the
		// most visible self-shadowing and deserves more precision than the far tail.
		float f = (float(j) + 0.55) / float(light_steps);
		float shaped = f * f * 0.65 + f * 0.35;
		vec3 light_p = p + sun_dir * (shaped * max_distance);
		optical_depth += cloud_density_coarse(light_p) * step_len;
		if (optical_depth * u_cloud_extinction > 9.0) break;
	}
	return exp(-optical_depth * u_cloud_extinction * 0.82);
}

float cloud_ray_jitter(vec3 d) {
	return fract(sin(dot(d, vec3(91.17, 37.53, 141.73))) * 43758.5453);
}

/// RGB = accumulated cloud radiance. A = background transmittance.
vec4 raymarch_clouds(
	vec3 origin,
	vec3 dir,
	vec3 sun_dir,
	vec3 sun_color,
	float sun_irradiance,
	int requested_steps,
	int requested_light_steps
) {
	if (u_cloud_enabled < 0.5) return vec4(0.0, 0.0, 0.0, 1.0);
	vec2 segment = cloud_layer_segment(origin, dir);
	if (segment.x > segment.y) return vec4(0.0, 0.0, 0.0, 1.0);

	int steps = clamp(requested_steps, 4, CLOUD_MAX_PRIMARY_STEPS);
	float step_len = (segment.y - segment.x) / float(steps);
	float jitter = cloud_ray_jitter(dir);
	float t = segment.x + step_len * mix(0.18, 0.82, jitter);
	float transmittance = 1.0;
	vec3 radiance = vec3(0.0);
	float mu = dot(dir, sun_dir);
	float phase = cloud_phase(mu);

	for (int i = 0; i < CLOUD_MAX_PRIMARY_STEPS; i++) {
		if (i >= steps || transmittance < 0.012 || t >= segment.y) break;
		vec3 p = origin + dir * t;
		// Fine erosion fades with distance. At the horizon a high-frequency sample
		// is sub-pixel detail and only adds noise/bandwidth.
		float detail_weight = 1.0 - smoothstep(42000.0, 115000.0, t);
		float density = cloud_density_at(p, detail_weight);
		if (density > 0.008) {
			float sample_alpha = 1.0 - exp(-density * u_cloud_extinction * step_len);
			vec3 up = normalize(p);
			float sun_mu = dot(up, sun_dir);
			float planet_vis = planet_direct_sun_visibility(p, sun_dir, u_planet_radius);
			float sun_air = mix(0.16, 1.0, smoothstep(-0.015, 0.32, sun_mu));
			vec3 sunset_tint = mix(
				vec3(1.00, 0.34, 0.10),
				vec3(1.00, 0.98, 0.94),
				smoothstep(-0.02, 0.28, sun_mu));
			float light_trans = cloud_sun_transmittance(p, sun_dir, requested_light_steps);

			// Beer-powder approximation: optically thin edge voxels do not receive the
			// same diffuse multiple-scatter boost as a developed droplet volume.
			float powder = 1.0 - exp(-density * u_cloud_extinction * step_len * 2.2);
			vec3 direct = sunset_tint * sun_color * sun_irradiance * phase
				* light_trans * planet_vis * sun_air * mix(0.58, 1.03, powder);

			float day = smoothstep(-0.16, 0.12, sun_mu);
			vec3 ambient = mix(
				vec3(0.0025, 0.0045, 0.010),
				vec3(0.050, 0.071, 0.102),
				day);
			vec3 multiple = sunset_tint * sun_color * sun_irradiance
				* (0.010 + 0.030 * (1.0 - light_trans)) * planet_vis * day;
			vec3 lighting = direct + ambient + multiple;

			radiance += transmittance * sample_alpha * lighting;
			transmittance *= 1.0 - sample_alpha;
		}
		t += step_len;
	}

	return vec4(radiance, clamp(transmittance, 0.0, 1.0));
}
