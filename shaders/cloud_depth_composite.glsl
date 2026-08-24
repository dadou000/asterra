#[compute]
#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(rgba16f, set = 0, binding = 0) uniform image2D color_image;
layout(set = 0, binding = 1) uniform sampler2D depth_texture;
layout(set = 0, binding = 2) uniform sampler3D shape_noise;
layout(set = 0, binding = 3) uniform sampler3D detail_noise;

layout(push_constant, std430) uniform Params {
	vec4 camera_planet_radius;
	vec4 camera_rotation;
	vec4 sun_dir_intensity;
	vec4 wind_steps;
	mat4 inv_projection;
} params;

const float PI = 3.14159265358979323846;
const float ATMOSPHERE_TOP = 60000.0;
const float CLOUD_BASE = 1100.0;
const float CLOUD_TOP = 6400.0;
const float CLOUD_COVERAGE = 0.52;
const float CLOUD_WEATHER_VARIATION = 0.30;
const float CLOUD_DENSITY = 1.0;
const float CLOUD_SHAPE_SCALE = 0.000055;
const float CLOUD_DETAIL_SCALE = 0.00042;
const float CLOUD_WEATHER_SCALE = 0.0000085;
const float CLOUD_DETAIL_STRENGTH = 0.36;
const float CLOUD_EXTINCTION = 0.00105;
const int CLOUD_MAX_PRIMARY_STEPS = 28;
const int CLOUD_MAX_LIGHT_STEPS = 4;
const int AIR_STEPS = 4;

vec3 quat_rotate(vec4 q, vec3 v) {
	return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

vec2 sphere_intersect(vec3 origin, vec3 dir, float radius) {
	float b = dot(dir, origin);
	float c = dot(origin, origin) - radius * radius;
	float disc = b * b - c;
	if (disc < 0.0) return vec2(1e30, -1e30);
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
}

float remap01(float v, float a, float b) {
	return clamp((v - a) / max(b - a, 1e-5), 0.0, 1.0);
}

float vertical_profile(float h, float cloud_type) {
	float bottom = smoothstep(0.0, 0.10, h);
	float top_start = mix(0.44, 0.72, cloud_type);
	float top = 1.0 - smoothstep(top_start, 1.0, h);
	return bottom * top;
}

vec3 surface_anchor(vec3 p, float radius) {
	return normalize(p) * (radius + 100.0);
}

float weather_field(vec3 surface_p, vec3 wind) {
	vec3 weather_wind = wind * 0.16;
	float a = textureLod(shape_noise,
		(surface_p + weather_wind) * CLOUD_WEATHER_SCALE, 0.0).r;
	float b = textureLod(shape_noise,
		(surface_p + weather_wind * 0.73) * (CLOUD_WEATHER_SCALE * 0.53)
		+ vec3(0.37, 0.11, 0.71), 0.0).r;
	return mix(a, b, 0.32);
}

float density_coarse(vec3 p, float radius, vec3 wind) {
	float altitude = length(p) - radius;
	if (altitude <= CLOUD_BASE || altitude >= CLOUD_TOP) return 0.0;
	float h = remap01(altitude, CLOUD_BASE, CLOUD_TOP);
	vec3 surface_p = surface_anchor(p, radius);
	float weather = textureLod(shape_noise,
		(surface_p + wind * 0.16) * CLOUD_WEATHER_SCALE, 0.0).r;
	float coverage = clamp(CLOUD_COVERAGE
		+ (weather - 0.5) * CLOUD_WEATHER_VARIATION, 0.06, 0.96);
	float threshold = 1.0 - coverage * 0.78;
	float macro = textureLod(shape_noise,
		(p + wind) * CLOUD_SHAPE_SCALE, 0.0).r;
	float body = smoothstep(threshold, min(threshold + 0.20, 0.99), macro);
	return body * vertical_profile(h, smoothstep(0.22, 0.82, weather))
		* CLOUD_DENSITY;
}

float density_at(vec3 p, float radius, vec3 wind, float detail_weight) {
	float altitude = length(p) - radius;
	if (altitude <= CLOUD_BASE || altitude >= CLOUD_TOP) return 0.0;

	float h = remap01(altitude, CLOUD_BASE, CLOUD_TOP);
	vec3 surface_p = surface_anchor(p, radius);
	float weather = weather_field(surface_p, wind);
	float coverage = clamp(CLOUD_COVERAGE
		+ (weather - 0.5) * CLOUD_WEATHER_VARIATION, 0.06, 0.96);
	float threshold = 1.0 - coverage * 0.78;

	vec3 shape_p = (p + wind) * CLOUD_SHAPE_SCALE;
	float n0 = textureLod(shape_noise, shape_p, 0.0).r;
	float n1 = textureLod(shape_noise,
		shape_p * 2.03 + vec3(0.19, 0.61, 0.43), 0.0).r;
	float macro = mix(n0, n1, 0.24);
	float body = smoothstep(threshold, min(threshold + 0.20, 0.99), macro);

	float type_noise = textureLod(shape_noise,
		(surface_p + wind * 0.11) * (CLOUD_WEATHER_SCALE * 0.67)
		+ vec3(0.73, 0.29, 0.13), 0.0).r;
	body *= vertical_profile(h, smoothstep(0.18, 0.86, type_noise));

	if (detail_weight > 0.001 && body > 0.015) {
		vec3 detail_p = (p + wind * 1.31) * CLOUD_DETAIL_SCALE;
		float detail = textureLod(detail_noise,
			detail_p + vec3(0.41, 0.17, 0.83), 0.0).r;
		float edge = 1.0 - smoothstep(0.30, 0.86, body);
		body = max(body - (1.0 - detail) * CLOUD_DETAIL_STRENGTH
			* detail_weight * (0.22 + 0.78 * edge), 0.0);
	}
	return smoothstep(0.015, 0.30, body) * CLOUD_DENSITY;
}

float hg(float mu, float g) {
	float gg = g * g;
	return (1.0 - gg) / (4.0 * PI
		* pow(max(1.0 + gg - 2.0 * g * mu, 1e-4), 1.5));
}

float cloud_phase(float mu) {
	return hg(mu, 0.65) * 0.86 + hg(mu, -0.24) * 0.14;
}

// Same astronomical occultation used by planet_lighting.gdshaderinc. The local
// geometric horizon drops below the tangent plane with altitude, while the exact
// ray/sphere test makes it impossible for direct Helion light to cross Asterra.
float planet_horizon_cosine(float sample_radius, float planet_radius) {
	float r = max(sample_radius, planet_radius + 0.01);
	float ratio = clamp(planet_radius / r, 0.0, 1.0);
	return -sqrt(max(1.0 - ratio * ratio, 0.0));
}

float planet_solar_clearance(vec3 p, vec3 sun_dir, float planet_radius) {
	float r = max(length(p), planet_radius + 0.01);
	vec3 up = p / r;
	return dot(up, normalize(sun_dir)) - planet_horizon_cosine(r, planet_radius);
}

float planet_sun_visibility(vec3 p, vec3 sun_dir, float planet_radius) {
	vec3 s = normalize(sun_dir);
	float r = length(p);
	if (r < 1.0) return 0.0;
	vec3 up = p / r;

	vec3 ro = p;
	if (r <= planet_radius + 0.5) {
		ro = up * (planet_radius + 0.5);
		r = planet_radius + 0.5;
	}

	float b = dot(ro, s);
	float c = dot(ro, ro) - planet_radius * planet_radius;
	float disc = b * b - c;
	if (disc > 0.0) {
		float t_near = -b - sqrt(disc);
		if (t_near > 1.0) return 0.0;
	}

	float horizon = planet_horizon_cosine(r, planet_radius);
	float mu = dot(up, s);
	const float SOLAR_EDGE = 0.0062;
	return smoothstep(horizon - SOLAR_EDGE, horizon + SOLAR_EDGE, mu);
}

// Indirect dusk light is tied to the sample's own curved-planet horizon instead
// of a fixed dot(up, sun) band. The small negative interval is only a few degrees
// below the cloud-level horizon; beyond it the cloud is genuinely on the night
// side and receives only the moonless night floor.
float cloud_twilight_weight(float solar_clearance) {
	float rise = smoothstep(-0.035, 0.004, solar_clearance);
	float fall = 1.0 - smoothstep(0.018, 0.12, solar_clearance);
	return rise * fall;
}

float sun_transmittance(vec3 p, vec3 sun_dir, float radius,
		vec3 wind, int light_steps) {
	vec2 hit = sphere_intersect(p, sun_dir, radius + CLOUD_TOP);
	float max_distance = min(max(hit.y, 0.0), 60000.0);
	if (max_distance <= 1.0) return 1.0;
	float step_len = max_distance / float(light_steps);
	float optical_depth = 0.0;
	for (int j = 0; j < CLOUD_MAX_LIGHT_STEPS; j++) {
		if (j >= light_steps) break;
		float f = (float(j) + 0.55) / float(light_steps);
		float shaped = f * f * 0.65 + f * 0.35;
		vec3 light_p = p + sun_dir * (shaped * max_distance);
		optical_depth += density_coarse(light_p, radius, wind) * step_len;
		if (optical_depth * CLOUD_EXTINCTION > 9.0) break;
	}
	return exp(-optical_depth * CLOUD_EXTINCTION * 0.82);
}

vec2 cloud_segment(vec3 origin, vec3 dir, float radius, float scene_distance) {
	float inner_radius = radius + CLOUD_BASE;
	float outer_radius = radius + CLOUD_TOP;
	vec2 outer_hit = sphere_intersect(origin, dir, outer_radius);
	if (outer_hit.y <= 0.0) return vec2(1e30, -1e30);

	float camera_radius = length(origin);
	float ray_start = max(outer_hit.x, 0.0);
	float ray_end = min(outer_hit.y, scene_distance);
	vec2 inner_hit = sphere_intersect(origin, dir, inner_radius);

	if (camera_radius < inner_radius) {
		ray_start = max(ray_start, inner_hit.y);
	} else if (camera_radius < outer_radius) {
		ray_start = 0.0;
		if (inner_hit.x > 0.0 && inner_hit.x < ray_end) ray_end = inner_hit.x;
	} else if (inner_hit.x > ray_start && inner_hit.x < ray_end) {
		ray_end = inner_hit.x;
	}

	vec2 ground_hit = sphere_intersect(origin, dir, radius);
	if (ground_hit.x > 0.0) ray_end = min(ray_end, ground_hit.x);
	if (ray_end <= ray_start) return vec2(1e30, -1e30);
	return vec2(ray_start, ray_end);
}

vec4 raymarch_clouds(vec3 origin, vec3 dir, float radius, float scene_distance,
		vec3 sun_dir, float sun_irradiance, vec3 wind, int requested_steps,
		out float first_cloud_distance) {
	first_cloud_distance = -1.0;
	vec2 segment = cloud_segment(origin, dir, radius, scene_distance);
	if (segment.x > segment.y) return vec4(0.0, 0.0, 0.0, 1.0);

	int steps = clamp(requested_steps, 6, CLOUD_MAX_PRIMARY_STEPS);
	int light_steps = clamp(2 + steps / 10, 2, CLOUD_MAX_LIGHT_STEPS);
	float step_len = (segment.y - segment.x) / float(steps);
	float t = segment.x + step_len * 0.5;
	float transmittance = 1.0;
	vec3 radiance = vec3(0.0);
	float phase = cloud_phase(dot(dir, sun_dir));

	for (int i = 0; i < CLOUD_MAX_PRIMARY_STEPS; i++) {
		if (i >= steps || transmittance < 0.012 || t >= segment.y) break;
		vec3 p = origin + dir * t;
		float detail_weight = 1.0 - smoothstep(28000.0, 76000.0, t);
		float density = density_at(p, radius, wind, detail_weight);
		if (density > 0.008) {
			if (first_cloud_distance < 0.0) first_cloud_distance = t;
			float sample_alpha = 1.0 - exp(-density * CLOUD_EXTINCTION * step_len);
			float planet_vis = planet_sun_visibility(p, sun_dir, radius);
			float solar_clearance = planet_solar_clearance(p, sun_dir, radius);

			// All sunrise/sunset response is measured from the cloud sample's true
			// spherical horizon. A 6 km cloud can therefore remain sunlit slightly
			// after the ground below it is dark, but cannot light up arbitrarily far
			// into the night hemisphere.
			float sun_air = mix(0.025, 1.0,
				smoothstep(-0.004, 0.28, solar_clearance));
			vec3 sunset_tint = mix(vec3(1.00, 0.34, 0.10),
				vec3(1.00, 0.98, 0.94),
				smoothstep(0.0, 0.24, solar_clearance));
			float light_trans = planet_vis > 0.001
				? sun_transmittance(p, sun_dir, radius, wind, light_steps)
				: 0.0;
			float powder = 1.0 - exp(-density * CLOUD_EXTINCTION * step_len * 2.2);
			vec3 direct = sunset_tint * sun_irradiance * phase
				* light_trans * planet_vis * sun_air * mix(0.58, 1.03, powder);

			float day = smoothstep(-0.002, 0.18, solar_clearance) * planet_vis;
			float twilight = cloud_twilight_weight(solar_clearance);
			vec3 ambient = vec3(0.00018, 0.00030, 0.00065);
			ambient += vec3(0.0045, 0.0065, 0.0120) * twilight;
			ambient += vec3(0.050, 0.071, 0.102) * day;
			vec3 multiple = sunset_tint * sun_irradiance
				* (0.010 + 0.030 * (1.0 - light_trans)) * day;
			vec3 lighting = direct + ambient + multiple;
			radiance += transmittance * sample_alpha * lighting;
			transmittance *= 1.0 - sample_alpha;
		}
		t += step_len;
	}
	return vec4(radiance, clamp(transmittance, 0.0, 1.0));
}

vec2 foreground_air_segment(vec3 camera_pos, vec3 dir, float first_t,
		float planet_radius) {
	vec2 hit = sphere_intersect(camera_pos, dir, planet_radius + ATMOSPHERE_TOP);
	if (hit.y <= 0.0) return vec2(1e30, -1e30);
	float ray_start = max(hit.x, 0.0);
	float ray_end = min(hit.y, first_t);
	if (ray_end <= ray_start) return vec2(1e30, -1e30);
	return vec2(ray_start, ray_end);
}

float foreground_air_transmittance(vec3 camera_pos, vec3 dir, float first_t,
		float planet_radius) {
	if (first_t <= 0.0) return 1.0;
	vec2 segment = foreground_air_segment(camera_pos, dir, first_t, planet_radius);
	if (segment.x > segment.y) return 1.0;

	float step_len = (segment.y - segment.x) / float(AIR_STEPS);
	float optical_length = 0.0;
	for (int i = 0; i < AIR_STEPS; i++) {
		float t = segment.x + (float(i) + 0.5) * step_len;
		float altitude = max(length(camera_pos + dir * t) - planet_radius, 0.0);
		float rayleigh = exp(-altitude / 8000.0);
		float mie = exp(-altitude / 1200.0);
		optical_length += (0.72 * rayleigh + 0.28 * mie) * step_len;
	}
	// Broadband approximation only for ordering the already-computed atmosphere
	// in front of the cloud. Vacuum contributes exactly zero because it is outside
	// foreground_air_segment().
	return exp(-optical_length * 1.55e-5);
}

vec3 foreground_atmosphere_restore(vec3 camera_pos, vec3 dir, float first_t,
		float cloud_transmittance, float air_transmittance,
		vec3 sun_dir, float planet_radius) {
	if (first_t <= 0.0 || cloud_transmittance > 0.999) return vec3(0.0);
	vec2 air_segment = foreground_air_segment(camera_pos, dir, first_t,
		planet_radius);
	if (air_segment.x > air_segment.y) return vec3(0.0);

	// Evaluate the solar state in the actual atmospheric section in front of the
	// cloud, never halfway through an orbital vacuum path.
	float air_mid_t = (air_segment.x + air_segment.y) * 0.5;
	vec3 mid_p = camera_pos + dir * air_mid_t;
	float solar_vis = planet_sun_visibility(mid_p, sun_dir, planet_radius);
	float solar_clearance = planet_solar_clearance(mid_p, sun_dir, planet_radius);
	float day = smoothstep(-0.002, 0.18, solar_clearance) * solar_vis;
	float twilight = cloud_twilight_weight(solar_clearance);

	vec3 local_up = normalize(mid_p);
	float elevation = clamp(dot(dir, local_up), -0.15, 1.0);
	vec3 day_haze = mix(vec3(0.30, 0.34, 0.38), vec3(0.19, 0.36, 0.52),
		smoothstep(-0.08, 0.25, elevation));
	vec3 dusk_haze = vec3(0.035, 0.013, 0.005);
	vec3 night_haze = vec3(0.00012, 0.00020, 0.00045);
	vec3 haze_color = night_haze + dusk_haze * twilight + day_haze * day;

	return haze_color * (1.0 - air_transmittance)
		* (1.0 - cloud_transmittance) * 0.55;
}

void main() {
	ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
	ivec2 size = imageSize(color_image);
	if (pixel.x >= size.x || pixel.y >= size.y) return;

	vec2 uv = (vec2(pixel) + vec2(0.5)) / vec2(size);
	float depth = textureLod(depth_texture, uv, 0.0).r;
	vec3 ndc = vec3(uv * 2.0 - 1.0, depth);
	vec4 view_h = params.inv_projection * vec4(ndc, 1.0);
	vec3 view_pos = view_h.xyz / max(abs(view_h.w), 1e-8) * sign(view_h.w);

	// Reverse-Z depth ~= 0 is untouched sky, not geometry at Camera3D.far.
	float scene_distance = depth <= 1e-6 ? 1e30 : length(view_pos);
	if (!(scene_distance > 0.0)) return;

	vec3 ray_view = normalize(view_pos);
	vec3 ray_world = normalize(quat_rotate(params.camera_rotation, ray_view));
	vec3 camera_planet = params.camera_planet_radius.xyz;
	float planet_radius = params.camera_planet_radius.w;
	vec3 sun_dir = normalize(params.sun_dir_intensity.xyz);
	float sun_irradiance = params.sun_dir_intensity.w;
	vec3 wind = params.wind_steps.xyz;
	int steps = int(clamp(floor(params.wind_steps.w + 0.5), 6.0,
		float(CLOUD_MAX_PRIMARY_STEPS)));

	float first_cloud_distance;
	vec4 cloud = raymarch_clouds(camera_planet, ray_world, planet_radius,
		max(scene_distance - 0.5, 0.0), sun_dir, sun_irradiance, wind, steps,
		first_cloud_distance);
	if (cloud.a > 0.9999) return;

	vec4 base = imageLoad(color_image, pixel);
	float air_t = foreground_air_transmittance(camera_planet, ray_world,
		first_cloud_distance, planet_radius);
	vec3 result = cloud.rgb * air_t + base.rgb * cloud.a;
	result += foreground_atmosphere_restore(camera_planet, ray_world,
		first_cloud_distance, cloud.a, air_t, sun_dir, planet_radius);
	imageStore(color_image, pixel, vec4(result, base.a));
}
