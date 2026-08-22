shader_type spatial;
render_mode depth_prepass_alpha, cull_back, diffuse_burley, specular_schlick_ggx;

// The coverage layer beneath the hair cards, and the whole appearance of hair
// too short to be worth a card.
//
// Its look has to change with length, because short hair does not look like a
// short version of long hair. Under about 10 mm there is no flow to see: a buzz
// cut is a field of cut shafts seen end on, dots of pigment with skin visible
// between them. Drawing the strand strip there gave directional streaks where
// there should be points, so a fade had no believable short end.
uniform sampler2D hair_strip : source_color, hint_default_transparent, filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D follicle_map : hint_default_transparent, filter_linear, repeat_enable;
uniform float follicle_cells = 28.0;
uniform float strip_variants = 3.0;
uniform float strand_detail : hint_range(0.5, 4.0) = 1.0;
uniform vec4 root_color : source_color = vec4(0.12, 0.055, 0.025, 1.0);
uniform float sheen : hint_range(0.0, 1.0) = 0.55;
uniform float sheen_roughness : hint_range(0.05, 1.0) = 0.52;
uniform float hair_length_range = 0.040;
uniform vec3 scalp_centre = vec3(0.0);
uniform float scalp_chart_scale = 14.5;

varying vec3 scalp_position;

// Triangle wave. Mirroring the strip instead of repeating it keeps the tiling
// continuous, so no seam appears where a tile would otherwise wrap from feathered
// tips back onto solid roots.
float ping_pong(float value) {
	return abs(fract(value * 0.5) * 2.0 - 1.0);
}

void vertex() {
	// The cap mesh lives in the head mount space, so VERTEX is already the scalp
	// position the chart is defined on.
	scalp_position = VERTEX;
}

void fragment() {
	// The chart is recomputed here rather than interpolated. A per-vertex UV is
	// linear inside a triangle, so its screen derivative is constant per triangle
	// and jumps at every edge, which facets the footprint test into visible
	// rectangles. Evaluated per fragment it is smooth, and so is fwidth.
	vec3 scalp_direction = normalize(scalp_position - scalp_centre);
	vec2 chart = vec2(scalp_direction.x, scalp_direction.z)
		/ max(1.0 + scalp_direction.y, 0.08) * scalp_chart_scale;
	float millimetres = COLOR.r * hair_length_range * 1000.0;
	float feather = COLOR.a;

	// Proximity to the nearest follicle. Thresholding it is what grows the dots:
	// at 1 mm they are pinpricks with skin all around, by 12 mm they have merged.
	// Follicles have to be visible far earlier than a linear ramp gives, so the
	// power front-loads the curve without letting it saturate early.
	float grow = pow(smoothstep(0.3, 11.0, millimetres), 0.45);
	float threshold = mix(0.82, 0.06, grow);
	vec4 follicle = texture(follicle_map, chart);
	float dot_field = follicle.a * mix(0.72, 1.0, follicle.g);

	// Flow starts as soon as a shaft has any length to lie over, and is plainly
	// readable by 5 mm: the follicle is smeared into a tail along the comb, so the
	// texture itself flows without needing a card.
	vec2 flow_uv = vec2(COLOR.g, COLOR.b) * 2.0 - 1.0;
	float streak = smoothstep(1.0, 8.0, millimetres);
	if (streak > 0.001) {
		float reach = streak * 0.25;
		for (int tap = 1; tap <= 5; tap++) {
			float along = float(tap) / 5.0;
			vec4 trail = texture(follicle_map, chart - flow_uv * reach * along);
			// The tail fades behind the follicle so a strand has a root and a tip.
			dot_field = max(dot_field, trail.a * mix(0.72, 1.0, trail.g) * (1.0 - along * 0.42));
		}
	}

	float soft_edge = mix(0.05, 0.30, grow);
	float resolved = smoothstep(threshold, threshold + soft_edge, dot_field);

	// Thresholding a texture only works while a dot still covers several pixels.
	// Past that the samples alias, and with mipmaps on they average to a grey that
	// sits below the threshold and vanishes entirely, which is what tripling the
	// follicle count did. So mipmaps are off, and beyond the point where dots can
	// be resolved this fades to the coverage they would average to.
	//
	// That average has to be derived, not guessed: the proximity field falls
	// linearly from the follicle to the cell edge, so the radius that survives is
	// (1 - threshold / peak) of the reach and the coverage is its disc. Peak is the
	// mean per-follicle strength and the soft edge folds in at half width. Getting
	// either wrong leaves a visible ring where resolved dots meet averaged tone.
	float radius = max(1.0 - (threshold + soft_edge * 0.5) / 0.86, 0.0);
	float mean_coverage = clamp(2.66 * radius * radius * (1.0 + streak * 1.6), 0.0, 1.0);
	float cells_per_pixel = max(fwidth(chart.x), fwidth(chart.y)) * follicle_cells;
	float detail = 1.0 - smoothstep(0.30, 1.05, cells_per_pixel);
	float dots = mix(mean_coverage, resolved, detail);

	// Only the dense root band of the strip is used: this layer is roots.
	vec2 strip_uv = vec2(ping_pong(chart.x * strand_detail) / strip_variants, ping_pong(chart.y * strand_detail) * 0.30);
	vec4 strip = texture(hair_strip, strip_uv);
	float strands = clamp(strip.a * 1.35, 0.0, 1.0);
	// The strip only has to cover the long end now that the stipple itself flows.
	float strand_mix = smoothstep(13.0, 24.0, millimetres);
	float coverage = mix(dots, max(dots, strands), strand_mix);

	// Below about half a millimetre there is no hair, only skin.
	float present = smoothstep(0.30, 1.10, millimetres);
	// Long hair wants a solid layer again: there it is the shadow between locks,
	// not the hair itself.
	float shape = mix(coverage, 1.0, strand_mix);

	// Opaque well inside the hairline, breaking up at the edge. The edge has to
	// break into whatever this length looks like: using the strand strip here drew
	// directional chevrons along the hairline of a fade, which is the one place a
	// flowing texture is most obviously wrong.
	float body = smoothstep(0.04, 0.24, feather) * shape;
	float rim = smoothstep(-0.02, 0.26, feather) * coverage;
	ALPHA = max(body, rim) * present;
	if (ALPHA < 0.004) {
		discard;
	}

	vec3 fiber_color = root_color.rgb * (0.80 + 0.40 * mix(strip.r, follicle.r, 1.0 - strand_mix));
	// A cut shaft end catches light; a shaft lying in a lock sits in its shadow.
	// Without this a buzz cut renders as black paint rather than as stubble.
	fiber_color = mix(fiber_color * 1.30, fiber_color, smoothstep(1.0, 9.0, millimetres));
	ALBEDO = fiber_color * mix(0.72, 1.0, strip.g);
	// Stubble scatters; a lock has a sheen band. Interpolate rather than pick one.
	ROUGHNESS = mix(0.86, sheen_roughness + 0.10, strand_mix);
	SPECULAR = sheen * mix(0.06, 0.20, strand_mix) * mix(0.45, 1.0, strip.g);
	ANISOTROPY = sheen * 0.6 * strand_mix;
	ANISOTROPY_FLOW = normalize(vec2(1.0, (strip.b - 0.5) * 0.40));
	BACKLIGHT = fiber_color * (0.30 + 0.25 * sheen);
}
