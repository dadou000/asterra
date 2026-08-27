// Shared water response: what the sea does to light, independent of which
// representation is drawing it.
//
// The optical surface is treated as an air/water dielectric. Resolved waves move
// the shading normal; wavelengths that are no longer resolved are represented by
// a microfacet slope distribution. Keeping those two roles separate is important:
// a real glitter path is the probability distribution of correctly-oriented
// surface facets, not a painted highlight.
//
// Requires planet_lighting.gdshaderinc for the day/night terminator.

const float WATER_IOR = 1.333;
const float WATER_F0 = 0.0203732;
// Godot's dielectric F0 is 0.16 * SPECULAR^2. This value makes its environment
// reflection path use the same ~2.04% normal-incidence reflectance as water.
const float WATER_GODOT_SPECULAR = 0.35684;

// Exact unpolarised Fresnel for an air -> water interface. Schlick is a useful
// approximation for ordinary solids, but the ocean spends a lot of screen area
// close to grazing incidence, precisely where its error is easiest to see.
float water_fresnel(float cos_incident) {
	float ci = clamp(cos_incident, 0.0, 1.0);
	float eta = 1.0 / WATER_IOR;
	float sin_t2 = eta * eta * max(1.0 - ci * ci, 0.0);
	float ct = sqrt(max(1.0 - sin_t2, 0.0));
	float rs_num = ci - WATER_IOR * ct;
	float rs_den = ci + WATER_IOR * ct;
	float rp_num = WATER_IOR * ci - ct;
	float rp_den = WATER_IOR * ci + ct;
	float rs = rs_num / max(abs(rs_den), 1e-6);
	float rp = rp_num / max(abs(rp_den), 1e-6);
	return clamp(0.5 * (rs * rs + rp * rp), WATER_F0, 1.0);
}

// Isotropic GGX / Trowbridge-Reitz specular with height-correlated Smith
// visibility. Kept for representations that do not carry a wind frame.
// Returns D*V*NdotL; the caller supplies Fresnel.
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

float water_aniso_lambda(vec3 w, vec3 n, vec3 t, vec3 b,
		float alpha_t, float alpha_b) {
	float ndotw = max(dot(n, w), 1e-5);
	float tw = dot(t, w);
	float bw = dot(b, w);
	float scaled = (alpha_t * tw) * (alpha_t * tw)
		+ (alpha_b * bw) * (alpha_b * bw);
	return 0.5 * (sqrt(1.0 + scaled / max(ndotw * ndotw, 1e-8)) - 1.0);
}

// Full direct-light ocean BRDF, including exact Fresnel. `t` is the along-wind
// axis. Cox-Munk observations show a wider along-wind than cross-wind slope PDF
// at useful wind speeds; `stretch` preserves lobe area while changing that ratio.
// The Fresnel angle is V.H, not N.V: using N.V on a rough surface makes grazing
// facets much too bright and was a major cause of broad white sunset bands.
float water_specular_direct_anisotropic(vec3 n, vec3 t_hint, vec3 v, vec3 l,
		float roughness, float stretch) {
	float ndotv = max(dot(n, v), 0.0);
	float ndotl = max(dot(n, l), 0.0);
	if (ndotv <= 1e-5 || ndotl <= 1e-5) return 0.0;

	vec3 t = t_hint - n * dot(t_hint, n);
	float t2 = dot(t, t);
	if (t2 < 1e-8) {
		vec3 fallback = abs(n.y) < 0.85 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
		t = cross(fallback, n);
		t2 = max(dot(t, t), 1e-8);
	}
	t *= inversesqrt(t2);
	vec3 b = normalize(cross(n, t));

	float alpha = max(roughness * roughness, 0.0025);
	float s = clamp(stretch, 0.72, 1.38);
	float alpha_t = clamp(alpha * s, 0.0025, 0.60);
	float alpha_b = clamp(alpha / s, 0.0025, 0.60);

	vec3 h = normalize(v + l);
	float hx = dot(h, t);
	float hy = dot(h, b);
	float hz = max(dot(h, n), 0.0);
	float ellipsoid = (hx / alpha_t) * (hx / alpha_t)
		+ (hy / alpha_b) * (hy / alpha_b) + hz * hz;
	float dist = 1.0 / max(PI * alpha_t * alpha_b * ellipsoid * ellipsoid, 1e-8);

	float lambda_v = water_aniso_lambda(v, n, t, b, alpha_t, alpha_b);
	float lambda_l = water_aniso_lambda(l, n, t, b, alpha_t, alpha_b);
	float smith_g = 1.0 / max(1.0 + lambda_v + lambda_l, 1e-5);
	float vis = smith_g / max(4.0 * ndotv * ndotl, 1e-5);
	float fres = water_fresnel(max(dot(v, h), 0.0));
	return dist * vis * fres * ndotl;
}

// Analytic sky radiance retained for the older transparent-water representation.
// The production opaque ocean uses Godot's HDR sky radiance cubemap instead, so
// the sun disc is not counted twice.
vec3 water_sky_radiance(vec3 dir, vec3 up, vec3 sun_dir) {
	float horizon = 1.0 - clamp(dot(dir, up), 0.0, 1.0);
	float mu = dot(dir, normalize(sun_dir));
	vec3 zenith = vec3(0.052, 0.108, 0.225);
	vec3 haze = vec3(0.285, 0.345, 0.430);
	vec3 col = mix(zenith, haze, pow(horizon, 2.2));
	col += vec3(0.60, 0.48, 0.33) * pow(max(mu, 0.0), 8.0) * 0.55;
	float day = smoothstep(-0.14, 0.22, dot(up, normalize(sun_dir)));
	float dusk = smoothstep(-0.22, -0.02, dot(up, normalize(sun_dir)))
		* (1.0 - smoothstep(-0.02, 0.20, dot(up, normalize(sun_dir))));
	return col * day + vec3(0.16, 0.09, 0.05) * dusk;
}

// Zonal (east-west), meridional (north-south) surface wind in m/s. This is the
// same three-cell circulation approximation used by the generator, but unlike
// the old speed-only helper it preserves hemisphere-correct meridional direction.
vec2 water_wind_components(vec3 up) {
	float lat_signed = degrees(asin(clamp(up.y, -1.0, 1.0)));
	float band = abs(lat_signed);
	float hemi = lat_signed >= 0.0 ? 1.0 : -1.0;
	float u;
	float v;
	if (band < 30.0) {
		float s = sin(PI * band / 30.0);
		u = -6.5 * s;
		v = -2.6 * s * hemi;
	} else if (band < 60.0) {
		float s = sin(PI * (band - 30.0) / 30.0);
		u = 9.5 * s;
		v = 1.8 * s * hemi;
	} else {
		float s = sin(PI * (band - 60.0) / 30.0);
		u = -5.0 * s;
		v = -1.2 * s * hemi;
	}
	return vec2(u, v);
}

float water_wind_speed(vec3 up) {
	return length(water_wind_components(normalize(up)));
}

vec3 water_wind_direction(vec3 up) {
	vec3 n = normalize(up);
	vec3 east = cross(vec3(0.0, 1.0, 0.0), n);
	float e2 = dot(east, east);
	if (e2 < 1e-8) {
		east = cross(vec3(1.0, 0.0, 0.0), n);
		e2 = max(dot(east, east), 1e-8);
	}
	east *= inversesqrt(e2);
	vec3 north = normalize(cross(n, east));
	vec2 wind = water_wind_components(n);
	if (dot(wind, wind) < 1e-6) return east;
	return normalize(east * wind.x + north * wind.y);
}

// Cox-Munk clean-ocean mean-square slope is approximately
// MSS = 0.003 + 0.00512 U10. GGX has heavier tails than their near-Gaussian PDF,
// so matching the central glitter lobe uses a fitted fraction of sqrt(MSS). The
// calm blend preserves genuinely glassy doldrum patches instead of forcing the
// finite Cox-Munk intercept onto zero-wind water.
float water_roughness_from_wind(float wind_speed, float gust) {
	float w = max(wind_speed * (0.72 + 0.56 * gust), 0.0);
	float mss = 0.003 + 0.00512 * w;
	float fitted_alpha = clamp(0.43 * sqrt(max(mss, 0.0)), 0.004, 0.19);
	float wind_blend = smoothstep(0.45, 2.8, w);
	float alpha = mix(0.004, fitted_alpha, wind_blend);
	return sqrt(alpha);
}

// Aspect stretch for the direct-sun glitter PDF. Cox-Munk component variances
// are sigma_along^2 ~= 0.00316 U and sigma_cross^2 ~= 0.003 + 0.00192 U.
// Fourth-root conversion gives a GGX-axis stretch whose width ratio follows the
// square-root slope-standard-deviation ratio while preserving alpha_x*alpha_y.
float water_cx_munk_stretch(float wind_speed) {
	float w = max(wind_speed, 0.5);
	float along_var = max(0.00316 * w, 0.001);
	float cross_var = max(0.003 + 0.00192 * w, 0.001);
	float ratio = clamp(along_var / cross_var, 0.45, 2.20);
	float stretch = pow(ratio, 0.25);
	return mix(1.0, stretch, smoothstep(2.0, 5.0, wind_speed));
}

// Water stops optically revealing its bottom on scales of tens of metres, not
// hundreds. These transitions are deliberately wavelength-agnostic because the
// renderer does not yet sample bottom albedo, but their depths follow clear-water
// visibility much more closely than the former 60..900 m deep-water fade.
vec3 water_depth_tint(float depth, vec3 shoal, vec3 shelf, vec3 deep) {
	float shelf_t = smoothstep(0.35, 12.0, depth);
	float deep_t = smoothstep(18.0, 85.0, depth);
	vec3 col = mix(shoal, shelf, shelf_t);
	return mix(col, deep, deep_t);
}