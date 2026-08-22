shader_type spatial;
render_mode cull_disabled, depth_prepass_alpha, diffuse_burley, specular_schlick_ggx;

uniform sampler2D guide_motion_texture : filter_nearest, repeat_disable;
uniform float guide_count = 96.0;
uniform float guide_segment_count = 16.0;
uniform float curl : hint_range(0.0, 1.0) = 0.1;
uniform float gradient_bias : hint_range(0.2, 4.0) = 1.35;
uniform vec4 root_color : source_color = vec4(0.12, 0.055, 0.025, 1.0);
uniform vec4 tip_color : source_color = vec4(0.30, 0.16, 0.08, 1.0);
uniform sampler2D hair_strip : source_color, hint_default_transparent, filter_linear_mipmap_anisotropic, repeat_disable;
uniform float strip_variants = 3.0;
uniform sampler2D head_field_texture : filter_linear, repeat_enable;
uniform mat4 world_to_mount;
uniform vec3 head_field_center = vec3(0.0);
uniform float head_field_margin = 0.001;
uniform float head_face_shield = 0.027;
uniform float head_lateral_push = 0.040;
uniform float front_sign = 1.0;
uniform float sheen : hint_range(0.0, 1.0) = 0.55;
uniform float sheen_roughness : hint_range(0.05, 1.0) = 0.52;

varying float card_phase;

float instance_hash(float value) {
	return fract(sin(value * 12.9898 + 78.233) * 43758.5453);
}

// Radius of the head in a given direction, out of the field baked from the mesh.
float head_field_radius(vec3 direction) {
	float azimuth = (atan(direction.x, direction.z) + PI) / TAU;
	float elevation = acos(clamp(direction.y, -1.0, 1.0)) / PI;
	// Azimuth wraps, elevation must not: a bilinear tap across the pole would
	// blend the crown into the chin.
	return texture(head_field_texture, vec2(azimuth, clamp(elevation, 0.011, 0.989))).r;
}

float head_face_weight(vec3 direction) {
	float forward = direction.z * front_sign;
	// Must match _face_weight() on the CPU. Ramping this in across the forehead
	// lifted the fringe off the skull and creased it.
	float below_brow = smoothstep(0.15, -0.17, direction.y);
	return smoothstep(0.10, 0.66, forward) * below_brow;
}

vec3 sample_guide(float guide_normalized, float strand_t) {
	float guide_x = (floor(guide_normalized * (guide_count - 1.0) + 0.5) + 0.5) / guide_count;
	float segment_position = clamp(strand_t, 0.0, 1.0) * (guide_segment_count - 1.0);
	float segment_a = floor(segment_position);
	float segment_b = min(segment_a + 1.0, guide_segment_count - 1.0);
	float row_a = (segment_a + 0.5) / guide_segment_count;
	float row_b = (segment_b + 0.5) / guide_segment_count;
	vec3 motion_a = textureLod(guide_motion_texture, vec2(guide_x, row_a), 0.0).rgb;
	vec3 motion_b = textureLod(guide_motion_texture, vec2(guide_x, row_b), 0.0).rgb;
	return mix(motion_a, motion_b, fract(segment_position));
}

void vertex() {
	float strand_t = clamp(UV.y, 0.0, 1.0);
	// A card only covers part of its guide, so it must sample only that part.
	// g = length as a fraction of the groom length, a = width as a multiple of the
	// nominal card width.
	float length_scale = max(INSTANCE_CUSTOM.g, 0.001);
	float width_scale = max(INSTANCE_CUSTOM.a, 0.001);
	vec3 guide_motion = sample_guide(INSTANCE_CUSTOM.r, strand_t * length_scale);
	// The texture stores displacement in the guide's normalized root frame, and
	// the instance basis re-applies its own scale on the way out. Divide it back
	// out so a short wide base card bends by the same world distance as a long
	// thin one instead of a fraction of it.
	guide_motion.x /= width_scale;
	guide_motion.z /= width_scale;
	guide_motion.y /= length_scale;
	VERTEX += guide_motion;

	float phase = instance_hash(float(INSTANCE_ID) + 0.37) * TAU;
	VERTEX.x += sin(strand_t * TAU * (0.55 + curl * 1.8) + phase) * curl * 0.018 * strand_t;
	VERTEX.z += cos(strand_t * TAU * (0.42 + curl * 1.4) + phase) * curl * 0.012 * strand_t;
	card_phase = phase;

	// Face collision, per rendered vertex. The guides are solved on the CPU, but
	// a card traces its own path from its own root, so the only place every piece
	// of drawn hair can be kept out of the face is here.
	mat4 to_mount = world_to_mount * MODEL_MATRIX;
	vec3 mount_position = (to_mount * vec4(VERTEX, 1.0)).xyz;
	vec3 field_relative = mount_position - head_field_center;
	float field_distance = length(field_relative);
	if (field_distance > 0.0001) {
		vec3 field_direction = field_relative / field_distance;
		// Gate by position ALONG the strand. A root is attached to the scalp and
		// must never be shoved off it — pushing near-root geometry is what creased
		// the fringe and exposed the cap underneath. Hair that has travelled far
		// enough to reach the face gets the full shield.
		float face = head_face_weight(field_direction) * smoothstep(0.04, 0.28, strand_t);
		float surface = head_field_radius(field_direction) + head_field_margin + face * head_face_shield;
		if (field_distance < surface) {
			vec3 target = field_direction * surface;
			if (face > 0.001) {
				// Straight out of a face is straight forwards, which just parks the
				// hair in front of it. Hair parts by sliding sideways, so add the
				// tangential push, biased to the side the card is rooted on.
				float side = (field_relative.x + (INSTANCE_CUSTOM.b - 0.5) * 0.02) >= 0.0 ? 1.0 : -1.0;
				vec3 lateral = vec3(side, 0.0, 0.0);
				lateral -= field_direction * dot(lateral, field_direction);
				// Scale by how deep the vertex actually is. A constant shove gives a
				// card that barely grazes the shield the same push as a buried one,
				// which flings single cards out sideways into the air.
				float depth = clamp((surface - field_distance) / max(head_face_shield, 0.001), 0.0, 1.0);
				if (dot(lateral, lateral) > 0.0001) {
					target += normalize(lateral) * face * head_lateral_push * depth;
				}
			}
			// Back into model space. The card basis is orthogonal with per-axis
			// scale, so each component is a projection over its own squared length.
			vec3 delta = target - field_relative;
			vec3 basis_x = to_mount[0].xyz;
			vec3 basis_y = to_mount[1].xyz;
			vec3 basis_z = to_mount[2].xyz;
			VERTEX += vec3(dot(delta, basis_x) / dot(basis_x, basis_x),
				dot(delta, basis_y) / dot(basis_y, basis_y),
				dot(delta, basis_z) / dot(basis_z, basis_z));
		}
	}

	// Hair reads almost entirely through the sheen band that runs across the
	// locks, and an anisotropic highlight needs the direction the strand actually
	// points. The card is modelled along +Y, so differencing the guide gives the
	// bent direction rather than the modelled one.
	float ahead = min(strand_t + 0.06, 1.0);
	vec3 forward = sample_guide(INSTANCE_CUSTOM.r, ahead * length_scale);
	forward.x /= width_scale;
	forward.z /= width_scale;
	forward.y /= length_scale;
	vec3 along = vec3(0.0, ahead - strand_t, 0.0) + forward - guide_motion;
	// NORMAL/TANGENT/BINORMAL are in model space here; Godot applies MODELVIEW
	// after vertex(). Transforming them here would apply it twice and hand the
	// anisotropic lobe a garbage frame.
	TANGENT = length(along) > 1e-7 ? normalize(along) : vec3(0.0, 1.0, 0.0);
	NORMAL = normalize(NORMAL - TANGENT * dot(NORMAL, TANGENT));
	BINORMAL = normalize(cross(NORMAL, TANGENT));
}

void fragment() {
	float strand_t = clamp(UV.y, 0.0, 1.0);
	// Each card draws one of several baked strip variants, optionally mirrored,
	// so neighbouring locks do not repeat the same silhouette.
	float variant = floor(instance_hash(card_phase * 3.1) * strip_variants);
	float flip = step(0.5, instance_hash(card_phase * 7.7));
	float strip_u = mix(UV.x, 1.0 - UV.x, flip);
	vec2 strip_uv = vec2((variant + strip_u) / strip_variants, strand_t);
	vec4 strip = texture(hair_strip, strip_uv);
	float coverage = strip.a;
	if (coverage < 0.015) {
		discard;
	}

	float gradient_t = pow(strand_t, max(gradient_bias, 0.01));
	vec3 fiber_color = mix(root_color.rgb, tip_color.rgb, gradient_t);
	// Per-strand tint out of the strip, plus a per-card offset, so a lock is not
	// one flat colour.
	fiber_color *= 0.78 + 0.44 * strip.r;
	fiber_color *= 0.93 + 0.07 * sin(card_phase * 7.0);
	// Strands sitting deeper in the lock catch less light.
	float occlusion = mix(0.62, 1.05, strip.g);
	ALBEDO = fiber_color * occlusion;
	// Anisotropy stretches the GGX lobe along the strand, so the highlight forms
	// a band across the lock instead of a round dot. A card this wide can carry a
	// real one; the old 4 mm fibers could not. Keep the lobe broad: a tight one on
	// a wide card reads as wet plastic rather than hair.
	ROUGHNESS = mix(sheen_roughness, sheen_roughness + 0.14, strand_t);
	// Real hair carries its highlight a third of the way down, not on the scalp.
	// Letting it reach the roots is what turns the crown into a wet plastic sheet.
	float highlight_band = smoothstep(0.02, 0.34, strand_t);
	SPECULAR = sheen * 0.26 * mix(0.45, 1.0, strip.g) * highlight_band;
	ANISOTROPY = sheen * 0.65;
	// Shift the anisotropic direction per strand. Without this every fragment on a
	// card shares one lobe and the whole lock lights up as a single sheet; the
	// shift is what breaks the highlight into strands.
	ANISOTROPY_FLOW = normalize(vec2(1.0, (strip.b - 0.5) * 0.40));
	// A dark albedo is physically right for brown hair, so the mass would read as
	// a black blob on diffuse alone. Transmission through the lock is what gives
	// real hair its colour away from the highlight.
	BACKLIGHT = fiber_color * (0.45 + 0.35 * sheen) * mix(0.85, 1.6, strand_t);
	// Soft strand edges instead of a binary cut. depth_prepass_alpha lays down
	// depth from the solid core first, so the blend still sorts.
	ALPHA = clamp(coverage * 1.15, 0.0, 1.0);
}
