shader_type spatial;
render_mode blend_mix, cull_front, unshaded, shadows_disabled, ambient_light_disabled,
	depth_draw_never, depth_test_disabled;
/// The volumetric envelope of a gas giant (after Blackrack's EVE Raymarched
/// Volumetrics -- see C:/Users/david/Desktop/clouds). Between the solid core
/// (u_core_radius, rho ~= 1000 kg/m^3) and the cloud tops (u_cloud_top_radius ==
/// CelestialBody.radius_m).
///
/// gas_giant_shell.gd draws two inward-facing decks:
///   u_is_cloud < 0.5  -- ATMOSPHERE: a cheap analytic march of the smooth density
///     with a GREEN Rayleigh-ish in-scatter tint (u_atmo_beta). This is where a
///     gas giant's colour comes from -- not the clouds. Thickens toward the limb,
///     always closes the view (no black seams / no starfield), stays a lit haze
///     deep instead of going black.
///   u_is_cloud > 0.5  -- CLOUDS: grey (u_cloud_color), lit + self-shadowed. The
///     COVERAGE comes from a procedural equirect texture (u_coverage_tex: R bands
///     + swirls, G detail, B storms) sampled by (longitude, latitude) with an
///     animated flow offset, times an analytic altitude profile -- a texture fetch
///     per step instead of a stack of FBM octaves, so it is crisp AND cheap.

uniform vec3 u_body_center = vec3(0.0);
uniform vec3 u_sun_dir = vec3(1.0, 0.0, 0.0);
uniform vec3 u_atmo_beta : source_color = vec3(0.30, 1.0, 0.42);
uniform vec3 u_cloud_color : source_color = vec3(0.86, 0.86, 0.84);
uniform vec3 u_deep_color : source_color = vec3(0.04, 0.05, 0.03);
uniform sampler2D u_coverage_tex : source_color, filter_linear, repeat_enable;
uniform float u_core_radius = 1.0;
uniform float u_cloud_top_radius = 2.0;
uniform float u_layer_inner = 1.0;
uniform float u_layer_outer = 2.0;
uniform float u_rho_top = 0.18;
uniform float u_falloff_k = 1.0e-6;
uniform float u_extinction = 6.0e-7;
uniform float u_scatter = 1.0;
uniform float u_sun_intensity = 3.4;
uniform float u_ambient = 0.12;
uniform int u_steps = 24;
uniform float u_density_mul = 1.0;

uniform float u_is_cloud = 0.0;
uniform float u_deck_altitude = 0.9;   // shell fraction of the cloud concentration
uniform float u_deck_width = 0.12;     // half-width (shell fraction)
uniform float u_detail = 0.6;          // erosion strength
uniform float u_flow = 0.03;           // uv flow speed
uniform float u_cloud_phase = 0.0;

const int MAX_STEPS = 28;
const int SUN_STEPS = 3;

vec2 sphere_hit(vec3 ro, vec3 rd, float radius) {
	float b = dot(rd, ro);
	float c = dot(ro, ro) - radius * radius;
	float disc = b * b - c;
	if (disc < 0.0) { return vec2(1.0e30, -1.0e30); }
	float s = sqrt(disc);
	return vec2(-b - s, -b + s);
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

float shell01_of(float r) {
	return clamp((r - u_core_radius) / max(u_cloud_top_radius - u_core_radius, 1.0), 0.0, 1.0);
}

// Equirect UV of a unit direction.
vec2 lonlat_uv(vec3 d) {
	return vec2(atan(d.z, d.x) / TAU + 0.5, asin(clamp(d.y, -1.0, 1.0)) / PI + 0.5);
}

// --- cheap 3D value noise for the near cloud billows ------------------------
float h13(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.yzx + 33.33);
	return fract((p.x + p.y) * p.z);
}
float vn(vec3 x) {
	vec3 i = floor(x);
	vec3 f = fract(x);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(h13(i), h13(i + vec3(1, 0, 0)), f.x), mix(h13(i + vec3(0, 1, 0)), h13(i + vec3(1, 1, 0)), f.x), f.y),
		mix(mix(h13(i + vec3(0, 0, 1)), h13(i + vec3(1, 0, 1)), f.x), mix(h13(i + vec3(0, 1, 1)), h13(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
// Ridged/billow FBM -> cauliflower cloud tops.
float billow(vec3 p, int oct) {
	float v = 0.0;
	float a = 0.6;
	float tot = 0.0;
	for (int i = 0; i < 3; i++) {
		if (i >= oct) { break; }
		v += a * abs(vn(p) * 2.0 - 1.0);
		tot += a;
		p = p * 2.13 + vec3(7.0, 3.0, 5.0);
		a *= 0.5;
	}
	return 1.0 - v / max(tot, 1.0e-3);   // 0..1, high where the cells crest
}

// Grey-cloud density at a point inside the cloud deck: the procedural equirect
// coverage map (belts + swirls, painted by GasGiantModel) carved into puffy 3D
// billows by ridged value noise, so a near view reads as a real cloudscape.
float cloud_density(vec3 p, float r) {
	float shell01 = shell01_of(r);
	float alt = exp(-pow((shell01 - u_deck_altitude) / max(u_deck_width, 1.0e-3), 2.0));
	if (alt < 0.02) { return 0.0; }
	vec3 d = normalize(rotate_y(p, u_cloud_phase * 0.02));
	vec2 scroll = vec2(u_cloud_phase * u_flow * 0.02, 0.0);
	vec4 a = texture(u_coverage_tex, lonlat_uv(d) + scroll);
	vec4 b = texture(u_coverage_tex, lonlat_uv(rotate_y(d.zxy, 2.3)) * 3.0 - scroll * 1.7);
	// Belt coverage (softened from the sharp far-LOD mask) + storm ovals.
	float belt = a.r * 0.6 + 0.4 * (a.g * 0.6 + b.r * 0.4) + a.b * 0.45;
	// Puffy 3D structure: ridged noise at a few km scale, flowing with the deck.
	// Flatten the sample point toward the shell (kill most of the radial component)
	// so a near-tangent ray does not smear one noise column down the screen.
	vec3 rad = p / max(r, 1.0);
	vec3 fp = p - rad * (dot(p, rad) - u_cloud_top_radius * u_deck_altitude) * 0.92;
	vec3 np = (fp / (u_cloud_top_radius * 0.0055)) + vec3(u_cloud_phase * 0.4, 0.0, 0.0);
	float puff = billow(np, 3);
	float cov = belt * mix(0.12, 1.4, puff) * alt;
	cov -= (1.0 - cov) * (1.0 - puff) * (0.35 + 0.45 * u_detail);   // carve deep gaps
	cov = clamp(cov, 0.0, 1.0);
	return base_rho(r) * pow(cov, 1.05) * 4.2;
}

// Smooth-density sun occlusion: the terminator shape.
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

// Cheap cloud self-shadow: one ridged-noise tap a short way toward the sun --
// where the neighbour cell crests, this puff sits in its shadow. No extra march.
float cloud_light(vec3 p) {
	vec3 np = (p / (u_cloud_top_radius * 0.0055))
		+ u_sun_dir * 1.4 + vec3(u_cloud_phase * 0.4, 0.0, 0.0);
	float shade = billow(np, 2);            // 0..1, high at a crest ahead -> shadowed
	return sun_transmit(p) * (1.0 - 0.78 * smoothstep(0.3, 0.9, shade));
}

void fragment() {
	vec3 cam = INV_VIEW_MATRIX[3].xyz;
	vec3 frag_world = (INV_VIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;
	vec3 rd = normalize(frag_world - cam);
	vec3 ro = cam - u_body_center;
	float mu = dot(rd, u_sun_dir);
	bool clouds = u_is_cloud > 0.5;

	vec2 outer = sphere_hit(ro, rd, u_layer_outer);
	vec2 lin = sphere_hit(ro, rd, u_layer_inner);
	vec2 core = sphere_hit(ro, rd, u_core_radius);
	float t0 = max(outer.x, 0.0);
	float t1 = outer.y;
	if (core.x > 0.0) { t1 = min(t1, core.x); }
	if (clouds) {
		if (lin.x > 0.0) { t1 = min(t1, lin.x); }
		else if (lin.y > 0.0) { t0 = max(t0, lin.y); }
	}

	if (t1 <= t0) {
		ALBEDO = vec3(0.0);
		ALPHA = 0.0;
	} else {
		int steps = clouds ? clamp(u_steps, 4, MAX_STEPS) : 14;
		float span = t1 - t0;
		float shell_span = max(u_cloud_top_radius - u_core_radius, 1.0);
		float max_dt = shell_span * (clouds ? 0.03 : 0.16);
		float jitter = clouds
			? fract(sin(dot(FRAGCOORD.xy, vec2(12.9898, 78.233))) * 43758.5453) * 0.4
			: 0.0;
		float t = t0 + jitter * min(span / float(steps), max_dt);
		float phase = 0.24 + 0.55 * max(mu, 0.0) * max(mu, 0.0) + 0.14 * max(-mu, 0.0);
		float T = 1.0;
		vec3 acc = vec3(0.0);
		for (int i = 0; i < MAX_STEPS; i++) {
			if (i >= steps || t >= t1) { break; }
			float dt = min((t1 - t) / float(steps - i) * 1.7, max_dt);
			vec3 p = ro + rd * (t + dt * 0.5);
			t += dt;
			float r = length(p);
			float day = smoothstep(-0.45, 0.25, dot(p / max(r, 1.0), u_sun_dir));
			float depth01 = 1.0 - shell01_of(r);
			float sv = sun_transmit(p);

			if (clouds) {
				float rho = cloud_density(p, r);
				if (rho <= 1.0e-8) { continue; }
				float seg = exp(-min(rho * u_extinction * dt, 4.0));
				// Grey cloud: direct sun through the self-shadow march (puff relief)
				// + forward scatter, plus green-atmosphere skylight fill.
				float cl = cloud_light(p);
				// Strong direct term (bright crests) + a modest green-sky fill so the
				// shadowed sides stay readable but the puffs keep 3D relief.
				vec3 lit = u_cloud_color * (u_sun_intensity * 1.9 * pow(cl, 1.4) * phase * day);
				vec3 sky = mix(u_atmo_beta, vec3(1.0), 0.35) * u_cloud_color
					* (u_ambient * 0.75 * (0.25 + 0.75 * day) * mix(0.4, 1.0, sv));
				vec3 lum = (lit + sky) * mix(1.0, 0.5, depth01 * depth01);
				acc += T * min(lum, vec3(1.4)) * (1.0 - seg);
				T *= seg;
			} else {
				// Atmosphere: green Rayleigh-ish in-scatter along the ray. Extinction
				// scaled by beta luminance so a long limb path goes opaque.
				float rho = base_rho(r);
				float bl = dot(u_atmo_beta, vec3(0.3, 0.6, 0.1));
				float seg = exp(-min(rho * u_extinction * bl * 2.2 * dt, 4.0));
				vec3 insc = u_atmo_beta * (u_sun_intensity * sv * phase * day
					+ u_ambient * (1.6 + 1.2 * day));
				vec3 murk = mix(u_atmo_beta, u_deep_color, depth01);
				vec3 lum = mix(insc, murk * u_ambient * 2.0, depth01 * depth01)
					+ murk * 0.06;
				acc += T * min(lum, vec3(1.5)) * (1.0 - seg);
				T *= seg;
			}
			if (T < 0.004) { break; }
		}
		ALBEDO = vec3(1.0) - exp(-acc * u_scatter);
		ALPHA = clamp(1.0 - T, 0.0, 1.0);
	}
}
