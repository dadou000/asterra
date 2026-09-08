shader_type spatial;
render_mode blend_mix, cull_front, unshaded, shadows_disabled, ambient_light_disabled,
	depth_draw_never, depth_test_disabled;
/// The volumetric envelope of a gas giant, between its solid core (u_core_radius,
/// where rho ~= 1000 kg/m^3) and its cloud tops (u_cloud_top_radius ==
/// CelestialBody.radius_m).
///
/// gas_giant_shell.gd builds TWO inward-facing decks from GasGiantModel.decks():
///   - "fog": the whole envelope, core -> cloud tops. A cheap 6-step march of the
///     smooth analytic density only (NO noise): lit haze that fades from the tops
///     toward the dark core and always goes opaque, so the view is closed (no
///     black seams, no stars) at any depth for near-free cost.
///   - "cloud": a bounded slab in the upper envelope where the banding lives.
///     Detailed march (noise, erosion, self-shadowing) but only across its own
///     radial slice, so cost is bounded no matter how thick the envelope is.
/// Drawn fog-first, cloud over it. Above the cloud tops both are dormant and the
/// far-LOD sphere carries the look.

uniform vec3 u_body_center = vec3(0.0);         // gas giant centre in the render frame
uniform vec3 u_sun_dir = vec3(1.0, 0.0, 0.0);  // unit, toward the star
uniform vec3 u_haze_color : source_color = vec3(0.62, 0.68, 0.42);
uniform vec3 u_deep_color : source_color = vec3(0.05, 0.04, 0.02);
uniform float u_core_radius = 1.0;
uniform float u_cloud_top_radius = 2.0;
uniform float u_layer_inner = 1.0;
uniform float u_layer_outer = 2.0;
uniform float u_rho_top = 0.18;
uniform float u_falloff_k = 1.0e-6;
uniform float u_extinction = 5.5e-7;
uniform float u_scatter = 1.0;
uniform float u_sun_intensity = 3.6;
uniform float u_ambient = 0.13;
uniform int u_steps = 28;
uniform float u_density_mul = 1.0;

uniform float u_is_cloud = 0.0;        // 0 = fog deck (cheap), 1 = detailed cloud slab
uniform float u_band_count = 7.0;      // latitudinal band pairs
uniform float u_band_contrast = 0.6;
uniform float u_noise_freq = 46.0;     // swirl cells across a body radius
uniform float u_warp = 0.6;
uniform float u_detail = 0.8;
uniform float u_coverage = 0.5;
uniform float u_cloud_phase = 0.0;

const int MAX_STEPS = 28;
const int SUN_STEPS = 2;

vec2 sphere_hit(vec3 ro, vec3 rd, float radius) {
	float b = dot(rd, ro);
	float c = dot(ro, ro) - radius * radius;
	float disc = b * b - c;
	if (disc < 0.0) { return vec2(1.0e30, -1.0e30); }
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
}

float hash13(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.yzx + 33.33);
	return fract((p.x + p.y) * p.z);
}
float vnoise(vec3 x) {
	vec3 i = floor(x);
	vec3 f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	float n000 = hash13(i + vec3(0.0, 0.0, 0.0));
	float n100 = hash13(i + vec3(1.0, 0.0, 0.0));
	float n010 = hash13(i + vec3(0.0, 1.0, 0.0));
	float n110 = hash13(i + vec3(1.0, 1.0, 0.0));
	float n001 = hash13(i + vec3(0.0, 0.0, 1.0));
	float n101 = hash13(i + vec3(1.0, 0.0, 1.0));
	float n011 = hash13(i + vec3(0.0, 1.0, 1.0));
	float n111 = hash13(i + vec3(1.0, 1.0, 1.0));
	return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
		mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}
float fbm(vec3 p, int octaves) {
	float v = 0.0;
	float a = 0.5;
	float tot = 0.0;
	for (int i = 0; i < 4; i++) {
		if (i >= octaves) { break; }
		v += a * vnoise(p);
		tot += a;
		p = p * 2.03 + vec3(11.3, 7.1, 5.7);
		a *= 0.5;
	}
	return v / max(tot, 1.0e-4);
}
vec3 rotate_y(vec3 v, float a) {
	float c = cos(a);
	float s = sin(a);
	return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

// Smooth analytic density (kg/m^3): thin at the tops, WATER_DENSITY at the core.
float base_rho(float r) {
	float rr = clamp(r, u_core_radius, u_cloud_top_radius);
	return u_density_mul * u_rho_top * exp(u_falloff_k * (u_cloud_top_radius - rr));
}

// Cloud-slab coverage 0..1: latitudinal bands set where gas gathers; a
// domain-warped FBM erodes it into swirls with genuine holes. 3D so it billows.
float cloud_cov(vec3 p, float r) {
	vec3 q = rotate_y(p / (u_cloud_top_radius * 0.5), u_cloud_phase) * (u_noise_freq * 0.06);
	vec2 w = vec2(fbm(q + 13.1, 2), fbm(q * 1.3 + 47.7, 2)) - 0.5;
	vec3 wv = vec3(w, w.x - w.y);
	vec3 dir = normalize(p);
	float bands = 0.5 + 0.5 * sin((dir.y + w.y * u_warp * 0.55) * u_band_count * TAU);
	bands = mix(0.55, bands, u_band_contrast);
	float base = fbm(q * 2.3 + wv * (u_warp * 2.2), 3);
	float erode = fbm(q * 4.4 + wv * 1.4, 2);
	float cov = base * mix(0.42, 1.0, bands) - erode * (0.18 + 0.14 * u_detail);
	cov = smoothstep(u_coverage - 0.06, u_coverage + 0.10, cov + 0.24);
	return pow(cov, 0.7);
}

// Smooth-density sun occlusion only (cheap): the terminator shape, not per-cloud.
float sun_transmit(vec3 p) {
	vec2 top = sphere_hit(p, u_sun_dir, u_cloud_top_radius);
	float march = max(min(top.y, u_cloud_top_radius * 0.4), 0.0);
	if (march <= 0.0) { return 1.0; }
	float dt = march / float(SUN_STEPS);
	float od = 0.0;
	for (int i = 0; i < SUN_STEPS; i++) {
		od += base_rho(length(p + u_sun_dir * (float(i) + 0.5) * dt)) * dt;
	}
	return exp(-od * u_extinction * 1.3);
}

vec3 light_at(vec3 p, float mu, float sv, bool detailed) {
	float r = length(p);
	vec3 dir = p / max(r, 1.0);
	float day = smoothstep(-0.45, 0.25, dot(dir, u_sun_dir));
	float depth01 = clamp((u_cloud_top_radius - r)
		/ max(u_cloud_top_radius - u_core_radius, 1.0), 0.0, 1.0);
	// Cheap latitudinal band tint so even the smooth fog shows Jool stripes.
	float lat = rotate_y(dir, u_cloud_phase * 0.15).y;
	float band = mix(1.0, 0.5 + 0.5 * sin(lat * u_band_count * TAU),
		u_band_contrast * 0.5 + 0.25);
	vec3 murk = mix(u_haze_color, u_deep_color, depth01) * band;
	if (!detailed) {
		// Fog deck: thick coloured murk that dims toward the core but stays a
		// visible glowing haze -- direct sun is a top-up, the body is the fill.
		float lit = u_sun_intensity * 0.35 * sv * day;
		return murk * (u_ambient * (2.6 + 1.6 * day) + lit) * mix(1.0, 0.55, depth01 * depth01)
			+ mix(u_haze_color, u_deep_color, depth01) * 0.09;
	}
	// Cloud slab: strong directional light + forward scatter so lit billows pop
	// against self-shadowed ones -- the relief that reads as volume.
	float phase = 0.24 + 0.60 * max(mu, 0.0) * max(mu, 0.0) + 0.14 * max(-mu, 0.0);
	vec3 sunlit = u_haze_color * (u_sun_intensity * 1.7 * sv * phase * day);
	vec3 fill = murk * (u_ambient * (1.0 + 0.7 * day) * mix(0.45, 1.0, sv));
	return (sunlit + fill) * mix(1.0, 0.7, depth01 * depth01);
}

void fragment() {
	vec3 cam = INV_VIEW_MATRIX[3].xyz;
	vec3 frag_world = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	vec3 rd = normalize(frag_world - cam);
	vec3 ro = cam - u_body_center;
	float mu = dot(rd, u_sun_dir);
	bool detailed = u_is_cloud > 0.5;

	vec2 outer = sphere_hit(ro, rd, u_layer_outer);
	vec2 lin = sphere_hit(ro, rd, u_layer_inner);
	vec2 core = sphere_hit(ro, rd, u_core_radius);
	float t0 = max(outer.x, 0.0);
	float t1 = outer.y;
	if (core.x > 0.0) { t1 = min(t1, core.x); }
	if (detailed) {
		// Clip to the slab [u_layer_inner, u_layer_outer].
		if (lin.x > 0.0) { t1 = min(t1, lin.x); }
		else if (lin.y > 0.0) { t0 = max(t0, lin.y); }
	}

	if (t1 <= t0) {
		ALBEDO = vec3(0.0);
		ALPHA = 0.0;
	} else {
		int steps = detailed ? clamp(u_steps, 4, MAX_STEPS) : 12;
		float span = t1 - t0;
		float max_dt = (u_cloud_top_radius - u_core_radius) * (detailed ? 0.06 : 0.13);
		// Jitter only the noisy cloud slab -- the smooth analytic fog integrates
		// cleanly and jittering its coarse steps just makes salt-and-pepper grain.
		float jitter = detailed
			? fract(sin(dot(FRAGCOORD.xy, vec2(12.9898, 78.233))) * 43758.5453) * 0.5
			: 0.0;
		float t = t0 + jitter * min(span / float(steps), max_dt);
		float T = 1.0;
		vec3 acc = vec3(0.0);
		for (int i = 0; i < MAX_STEPS; i++) {
			if (i >= steps || t >= t1) { break; }
			float dt = min((t1 - t) / float(steps - i) * 1.7, max_dt);
			vec3 p = ro + rd * (t + dt * 0.5);
			t += dt;
			float rho = base_rho(length(p));
			if (detailed) { rho *= cloud_cov(p, length(p)) * 3.6; }
			if (rho > 1.0e-8) {
				// Clamp per-step opacity so one long step can't spike to white.
				float seg = exp(-min(rho * u_extinction * dt, 4.0));
				vec3 lum = min(light_at(p, mu, sun_transmit(p), detailed), vec3(1.4));
				acc += T * lum * (1.0 - seg);
				T *= seg;
				if (T < 0.004) { break; }
			}
		}
		ALBEDO = vec3(1.0) - exp(-acc * u_scatter);
		ALPHA = clamp(1.0 - T, 0.0, 1.0);
	}
}
