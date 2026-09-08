shader_type spatial;
render_mode blend_mix, cull_front, unshaded, shadows_disabled, ambient_light_disabled,
	depth_draw_never, depth_test_disabled;
/// The volumetric envelope of a gas giant, between its solid core (core_radius,
/// where rho ~= 1000 kg/m^3) and its cloud tops (cloud_top_radius ==
/// CelestialBody.radius_m).
///
/// Rendered as concentric BANDS (gas_giant_shell.gd): each instance of this
/// material marches only its own radial slice [u_layer_inner, u_layer_outer], and
/// the manager only makes visible the band(s) the camera is in or approaching. So
/// the per-frame raymarch cost is bounded regardless of how thick the envelope
/// is or how many gas giants are resident. Above the cloud tops every band is
/// hidden and the far-LOD sphere carries the look.
///
/// Single-scatter dust/haze: analytic exponential density (matches
/// GasGiantModel.density_at), exponential extinction, one sun term per step with
/// a short secondary march. GG3 adds the banded cloud decks; GG4 clamps to depth.

uniform vec3 u_body_center = vec3(0.0);        // gas giant centre in the render frame (moves with Frames rebase)
uniform vec3 u_sun_dir = vec3(1.0, 0.0, 0.0);  // unit, toward the star
uniform vec3 u_haze_color : source_color = vec3(0.55, 0.62, 0.36);
uniform float u_core_radius = 1.0;
uniform float u_cloud_top_radius = 2.0;
uniform float u_layer_inner = 1.0;            // this band's inner radius (>= core)
uniform float u_layer_outer = 2.0;            // this band's outer radius (<= cloud tops)
uniform float u_rho_top = 0.18;                // kg/m^3 at the cloud tops
uniform float u_falloff_k = 1.0e-6;           // per metre; rho = u_rho_top * exp(k * (cloud_top - r))
uniform float u_extinction = 1.6e-6;           // per metre per (kg/m^3)
uniform float u_scatter = 1.0;                 // in-scatter weight
uniform float u_sun_intensity = 14.0;          // envelope-top sunlight, linear
uniform float u_ambient = 0.12;               // faint fill so the night side is not pure black
uniform int u_steps = 16;                     // primary steps for THIS deck's bounded span
uniform float u_density_mul = 1.0;            // this deck's density scale (cloud decks > 1)

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

// Analytic envelope density (kg/m^3): thin at the cloud tops, liquid-water dense
// at the core. Matches GasGiantModel.density_at within the shell, scaled by this
// deck's density_mul (cloud decks are denser than the smooth haze base).
float density_at(float r) {
	float rr = clamp(r, u_core_radius, u_cloud_top_radius);
	return u_density_mul * u_rho_top * exp(u_falloff_k * (u_cloud_top_radius - rr));
}

// Gentle forward-biased phase, kept un-normalised so the haze reads as lit.
float phase_soft(float mu) {
	return 0.30 + 0.55 * max(mu, 0.0) * max(mu, 0.0);
}

// Optical depth from p toward the sun, out to the cloud tops (short march).
float sun_optical_depth(vec3 p, vec3 sdir) {
	vec2 top = sphere_hit(p, sdir, u_cloud_top_radius);
	float march = max(min(top.y, u_cloud_top_radius), 0.0);
	if (march <= 0.0) { return 0.0; }
	float dt = march / float(SUN_STEPS);
	float od = 0.0;
	for (int i = 0; i < SUN_STEPS; i++) {
		vec3 q = p + sdir * (float(i) + 0.5) * dt;
		od += density_at(length(q)) * dt;
	}
	return od;
}

void fragment() {
	vec3 cam = INV_VIEW_MATRIX[3].xyz;
	vec3 frag_world = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	vec3 rd = normalize(frag_world - cam);
	vec3 ro = cam - u_body_center;   // body-centred ray origin

	// Overall shell segment (cloud tops -> core).
	vec2 top = sphere_hit(ro, rd, u_cloud_top_radius);
	float t0 = max(top.x, 0.0);
	float t1 = top.y;
	vec2 core = sphere_hit(ro, rd, u_core_radius);
	if (core.x > 0.0) { t1 = min(t1, core.x); }

	// Restrict to THIS band: enter through its outer boundary, stop at its inner
	// boundary (or the core / the existing t1). The band's far side is covered by
	// the next band inward when the camera is deep enough for it to be active.
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
			float rho = density_at(r);
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
		ALBEDO = scattered * u_scatter;
		ALPHA = clamp(1.0 - transmittance, 0.0, 1.0);
	}
}
