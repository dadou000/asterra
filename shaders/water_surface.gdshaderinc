// Shared water response: what the sea does to light, independent of which
// representation is drawing it.
//
// The old specular was a Blinn-Phong lobe with a fixed exponent, which is why
// the sun arrived on the ocean as one large soft white blob rather than the
// elongated, wind-broken glitter path a real sea produces. Everything here is
// the physical version of that: a microfacet distribution whose width comes from
// the sea state, a Fresnel term that decides how much of the sky the surface
// hands back, and a sky radiance for it to hand back.
//
// Requires planet_lighting.gdshaderinc for the day/night terminator.

/// Schlick Fresnel for a water/air interface. Water is F0 = 0.02 face-on and a
/// near-perfect mirror at grazing incidence, which is the single most
/// recognisable thing about looking at a sea.
float water_fresnel(float ndotv) {
	float f = pow(1.0 - clamp(ndotv, 0.0, 1.0), 5.0);
	return 0.02 + 0.98 * f;
}

/// GGX / Trowbridge-Reitz specular with height-correlated Smith visibility.
/// Returns the full D*V*NdotL term; multiply by Fresnel and light colour.
///
/// The long tail of GGX is the point: it is what stretches the glint into a
/// streak pointing at the observer and makes the sea look wet instead of
/// plastic. A Phong lobe cannot produce that shape at any exponent.
float water_specular(vec3 n, vec3 v, vec3 l, float roughness) {
	float a = max(roughness * roughness, 2e-5);
	vec3 h = normalize(v + l);
	float ndoth = max(dot(n, h), 0.0);
	float ndotv = max(dot(n, v), 1e-4);
	float ndotl = max(dot(n, l), 0.0);
	if (ndotl <= 0.0) return 0.0;

	float a2 = a * a;
	float d = ndoth * ndoth * (a2 - 1.0) + 1.0;
	float dist = a2 / max(PI * d * d, 1e-8);

	float lv = ndotl * sqrt(ndotv * ndotv * (1.0 - a2) + a2);
	float ll = ndotv * sqrt(ndotl * ndotl * (1.0 - a2) + a2);
	float vis = 0.5 / max(lv + ll, 1e-5);
	return dist * vis * ndotl;
}

/// Analytic sky radiance in a given direction. Cheap on purpose: this is what a
/// reflection carries, and the reflected sky is a large, low-frequency thing
/// that does not repay a second scattering integral.
vec3 water_sky_radiance(vec3 dir, vec3 up, vec3 sun_dir) {
	float horizon = 1.0 - clamp(dot(dir, up), 0.0, 1.0);
	float mu = dot(dir, normalize(sun_dir));
	vec3 zenith = vec3(0.052, 0.108, 0.225);
	vec3 haze = vec3(0.285, 0.345, 0.430);
	vec3 col = mix(zenith, haze, pow(horizon, 2.2));
	// Forward-scattered aureole: the sky immediately around the sun is much
	// brighter than the rest of it, and the sea reflects that too.
	col += vec3(0.60, 0.48, 0.33) * pow(max(mu, 0.0), 8.0) * 0.55;
	// A reflection of the sky has to go out when the sky does.
	float day = smoothstep(-0.14, 0.22, dot(up, normalize(sun_dir)));
	float dusk = smoothstep(-0.22, -0.02, dot(up, normalize(sun_dir)))
		* (1.0 - smoothstep(-0.02, 0.20, dot(up, normalize(sun_dir))));
	return col * day + vec3(0.16, 0.09, 0.05) * dusk;
}

/// Surface wind speed, m/s, from the same three-cell circulation the generator
/// uses in pass_climate. Reproducing the profile rather than sampling a texture
/// keeps sea state consistent with the climate that produced it, and gives the
/// ocean the banding it should have: glassy doldrums and horse latitudes, rough
/// westerlies at forty degrees.
float water_wind_speed(vec3 up) {
	float band = degrees(asin(clamp(abs(up.y), 0.0, 1.0)));
	float u;
	float v;
	if (band < 30.0) {
		float s = sin(PI * band / 30.0);
		u = -6.5 * s;
		v = 2.6 * s;
	} else if (band < 60.0) {
		float s = sin(PI * (band - 30.0) / 30.0);
		u = 9.5 * s;
		v = 1.8 * s;
	} else {
		float s = sin(PI * (band - 60.0) / 30.0);
		u = -5.0 * s;
		v = 1.2 * s;
	}
	return length(vec2(u, v));
}

/// Perceptual GGX roughness from unresolved ocean slope statistics.
///
/// Godot's material `ROUGHNESS` is perceptual: the GGX alpha used by the BRDF is
/// roughness^2. Treating the wind-speed response itself as perceptual roughness
/// made a 10 m/s sea use alpha ~= 0.01, far too mirror-like. A real wind-roughened
/// sea has a much wider slope distribution. The approximation below stores that
/// distribution as GGX alpha first and only then converts to perceptual roughness.
/// Calm water therefore stays sharp, while moderate/high sea produces a broad,
/// broken sun-glitter path instead of coherent white stripes.
float water_roughness_from_wind(float wind_speed, float gust) {
	float w = max(wind_speed * (0.72 + 0.56 * gust), 0.0);
	float alpha = clamp(0.004 + 0.011 * w, 0.004, 0.215);
	return sqrt(alpha);
}

/// Depth colour. Shallow water over sediment is green-turquoise because the
/// bottom is still returning light; deep water is nearly black-blue because
/// nothing comes back at all.
vec3 water_depth_tint(float depth, vec3 shoal, vec3 shelf, vec3 deep) {
	float shelf_t = smoothstep(0.5, 45.0, depth);
	float deep_t = smoothstep(60.0, 900.0, depth);
	vec3 col = mix(shoal, shelf, shelf_t);
	return mix(col, deep, deep_t);
}
