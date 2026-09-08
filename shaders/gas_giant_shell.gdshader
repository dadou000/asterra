shader_type spatial;
render_mode blend_mix, cull_front, unshaded, shadows_disabled, ambient_light_disabled,
	depth_draw_never, depth_test_disabled;
/// The volumetric envelope of a gas giant, between its solid core (core_radius,
/// where rho ~= 1000 kg/m^3) and its cloud tops (cloud_top_radius ==
/// CelestialBody.radius_m).
///
/// Rendered as concentric DECKS (gas_giant_shell.gd, from GasGiantModel.decks()):
/// each instance of this material marches only its own radial slice
/// [u_layer_inner, u_layer_outer], and the manager only shows the deck(s) the
/// camera is in or approaching, so cost is bounded no matter the envelope
/// thickness or how many gas giants are resident.
///
/// haze decks: smooth analytic exponential density.
/// cloud decks (u_is_cloud > 0.5, GG3): the same density modulated by Jool-style
/// latitudinal bands + domain-warped FBM swirls, rotating about the spin axis
/// (u_cloud_phase, from the body's sidereal day). Single scatter with a short sun
/// march (haze-only, no cloud detail). Radial edges of every deck fade so the
/// layered hand-offs blend instead of seaming.

uniform vec3 u_body_center = vec3(0.0);        // gas giant centre in the render frame
uniform vec3 u_sun_dir = vec3(1.0, 0.0, 0.0);  // unit, toward the star
uniform vec3 u_haze_color : source_color = vec3(0.55, 0.62, 0.36);
uniform float u_core_radius = 1.0;
uniform float u_cloud_top_radius = 2.0;
uniform float u_layer_inner = 1.0;
uniform float u_layer_outer = 2.0;
uniform float u_rho_top = 0.18;
uniform float u_falloff_k = 1.0e-6;
uniform float u_extinction = 1.6e-6;
uniform float u_scatter = 1.0;
uniform float u_sun_intensity = 14.0;
uniform float u_ambient = 0.16;
uniform int u_steps = 16;
uniform float u_density_mul = 1.0;

// --- GG3 cloud-deck params (ignored when u_is_cloud < 0.5) ------------------
uniform float u_is_cloud = 0.0;
uniform float u_band_count = 6.0;
uniform float u_band_contrast = 0.5;
uniform float u_noise_freq = 9.0;
uniform float u_warp = 0.3;
uniform float u_coverage = 0.5;
uniform float u_edge_hardness = 0.5;
uniform float u_cloud_phase = 0.0;   // radians, rotation of this deck about +Y

const int MAX_STEPS = 24;
const int SUN_STEPS = 2;

vec2 sphere_hit(vec3 ro, vec3 rd, float radius) {
	float b = dot(rd, ro);
	float c = dot(ro, ro) - radius * radius;
	float disc = b * b - c;
	if (disc < 0.0) { return vec2(1.0e30, -1.0e30); }
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
}

// --- noise --------------------------------------------------------------------
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
float fbm(vec3 p) {
	float v = 0.0;
	float a = 0.5;
	for (int i = 0; i < 3; i++) {
		v += a * vnoise(p);
		p *= 2.03;
		a *= 0.5;
	}
	return v;
}

vec3 rotate_y(vec3 v, float a) {
	float c = cos(a);
	float s = sin(a);
	return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}

// Cloud coverage 0..1 at a shell point. `dir` is the unit direction (== normalised
// body-centred point). Latitudinal bands weaving + domain-warped FBM swirls.
float cloud_coverage(vec3 dir) {
	vec3 d = rotate_y(dir, u_cloud_phase);
	vec3 np = d * u_noise_freq;
	// One warp field reused for the bands and the turbulence (cheap).
	vec3 warp = vec3(fbm(np * 0.5 + 3.1), fbm(np * 0.5 + 7.7), fbm(np * 0.5 + 1.9)) - 0.5;
	float bands = 0.5 + 0.5 * sin(d.y * u_band_count * PI + warp.x * u_warp * 6.0);
	bands = mix(0.5, bands, u_band_contrast);
	float turb = fbm(np + warp * u_warp * 4.0);
	float detail = fbm(np * 2.7 + warp * 1.5);
	float cov = bands * (0.40 + 0.60 * turb) * mix(1.0, detail, 0.35);
	float soft = (1.0 - u_edge_hardness) * 0.32 + 0.03;
	return smoothstep(u_coverage - soft, u_coverage + 0.05, cov);
}

// `clouds`: apply the cloud-deck coverage modulation. The secondary sun march
// passes false -- sun visibility does not need per-cloud detail and it would
// triple the noise cost.
float density_at(float r, vec3 p, bool clouds) {
	float rr = clamp(r, u_core_radius, u_cloud_top_radius);
	float rho = u_density_mul * u_rho_top * exp(u_falloff_k * (u_cloud_top_radius - rr));
	// Fade every deck out at its own radial edges so stacked decks blend rather
	// than seam. Capped so a thick haze deck is not gutted by its own fade.
	float edge = clamp((u_layer_outer - u_layer_inner) * 0.15, 2000.0, u_cloud_top_radius * 0.02);
	float lo_fade = smoothstep(u_layer_inner, u_layer_inner + edge, r);
	float hi_fade = 1.0 - smoothstep(u_layer_outer - edge, u_layer_outer, r);
	rho *= lo_fade * hi_fade;
	if (clouds && u_is_cloud > 0.5 && r > 1.0) {
		rho *= mix(0.05, 1.0, cloud_coverage(p / r));
	}
	return rho;
}

float phase_soft(float mu) {
	return 0.30 + 0.55 * max(mu, 0.0) * max(mu, 0.0);
}

float sun_optical_depth(vec3 p, vec3 sdir) {
	vec2 top = sphere_hit(p, sdir, u_cloud_top_radius);
	float march = max(min(top.y, u_cloud_top_radius), 0.0);
	if (march <= 0.0) { return 0.0; }
	float dt = march / float(SUN_STEPS);
	float od = 0.0;
	for (int i = 0; i < SUN_STEPS; i++) {
		vec3 q = p + sdir * (float(i) + 0.5) * dt;
		od += density_at(length(q), q, false) * dt;
	}
	return od;
}

void fragment() {
	vec3 cam = INV_VIEW_MATRIX[3].xyz;
	vec3 frag_world = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	vec3 rd = normalize(frag_world - cam);
	vec3 ro = cam - u_body_center;

	vec2 top = sphere_hit(ro, rd, u_cloud_top_radius);
	float t0 = max(top.x, 0.0);
	float t1 = top.y;
	vec2 core = sphere_hit(ro, rd, u_core_radius);
	if (core.x > 0.0) { t1 = min(t1, core.x); }

	vec2 lo = sphere_hit(ro, rd, u_layer_outer);
	if (lo.y > 0.0) {
		t0 = max(t0, max(lo.x, 0.0));
		t1 = min(t1, lo.y);
	}
	vec2 li = sphere_hit(ro, rd, u_layer_inner);
	if (li.x > 0.0) { t1 = min(t1, li.x); }

	if (t1 <= t0) {
		ALBEDO = vec3(0.0);
		ALPHA = 0.0;
	} else {
		int steps = clamp(u_steps, 4, MAX_STEPS);
		float span = t1 - t0;
		float dt = span / float(steps);
		float mu = dot(rd, u_sun_dir);
		float phase = phase_soft(mu);
		float transmittance = 1.0;
		vec3 scattered = vec3(0.0);
		for (int i = 0; i < MAX_STEPS; i++) {
			if (i >= steps) { break; }
			vec3 p = ro + rd * (t0 + (float(i) + 0.5) * dt);
			float r = length(p);
			float rho = density_at(r, p, true);
			if (rho > 1.0e-7) {
				float sigma = rho * u_extinction;
				float seg_tr = exp(-sigma * dt);
				float sun_od = sun_optical_depth(p, u_sun_dir);
				float sun_vis = exp(-sun_od * u_extinction);
				float lit = smoothstep(-0.15, 0.25, dot(normalize(p), u_sun_dir));
				vec3 light = u_haze_color * (u_sun_intensity * sun_vis * phase * lit + u_ambient);
				scattered += transmittance * light * (1.0 - seg_tr);
				transmittance *= seg_tr;
				if (transmittance < 0.003) { break; }
			}
		}
		ALBEDO = scattered * u_scatter;
		ALPHA = clamp(1.0 - transmittance, 0.0, 1.0);
	}
}
