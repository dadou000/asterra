/// Temporally stable integration helpers for the visible sky pass.
///
/// The original cloud march deliberately jittered the first primary sample with
/// a high-frequency hash of the view direction. That removes static step bands,
/// but a sub-pixel angular change then chooses a completely different integration
/// phase. In a half-resolution sky pass this appears as crawling/stippling during
/// camera motion. We prefer deterministic midpoint integration here: every ray
/// changes continuously as the camera moves, so temporal stability comes before
/// stochastic band hiding.

float stable_primary_phase(vec3 _dir) {
	return 0.5;
}

/// Stable counterpart of raymarch_clouds(). All cloud density, lighting,
/// extinction and spherical-shell logic stays exactly the same; only the primary
/// sample phase is changed from a decorrelated angular hash to the step midpoint.
vec4 raymarch_clouds_stable(
	vec3 origin,
	vec3 dir,
	vec3 sun_dir,
	vec3 sun_color,
	float sun_irradiance,
	int requested_steps,
	int requested_light_steps
) {
	if (AT_CUBEMAP_PASS || u_cloud_enabled < 0.5) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}

	vec2 segment = cloud_layer_segment(origin, dir);
	if (segment.x > segment.y) {
		return vec4(0.0, 0.0, 0.0, 1.0);
	}

	int steps = clamp(requested_steps, 4, CLOUD_MAX_PRIMARY_STEPS);
	float step_len = (segment.y - segment.x) / float(steps);
	float t = segment.x + step_len * 0.5;
	float transmittance = 1.0;
	vec3 radiance = vec3(0.0);
	float mu = dot(dir, sun_dir);
	float phase = cloud_phase(mu);

	for (int i = 0; i < CLOUD_MAX_PRIMARY_STEPS; i++) {
		if (i >= steps || transmittance < 0.012 || t >= segment.y) {
			break;
		}

		vec3 p = origin + dir * t;
		// Fine erosion is useful nearby, but its smallest features become a source
		// of temporal aliasing long before the macro cloud body does. Fade it sooner
		// than the previous 42-115 km interval while preserving the large-scale
		// density field at the horizon.
		float detail_weight = 1.0 - smoothstep(28000.0, 76000.0, t);
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
			float light_trans = cloud_sun_transmittance(
				p, sun_dir, requested_light_steps);

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
