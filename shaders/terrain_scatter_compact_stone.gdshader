shader_type spatial;
render_mode diffuse_burley, specular_schlick_ggx;

// 0 = geological / scree stone, 1 = river / depositional stone.
uniform int u_stone_kind = 0;

varying vec3 v_stone_color;
varying float v_stone_roughness;

vec3 rock_color(float rock_id) {
	if (rock_id < 0.5) return vec3(0.335, 0.305, 0.275);
	if (rock_id < 1.5) return vec3(0.085, 0.082, 0.080);
	if (rock_id < 2.5) return vec3(0.105, 0.104, 0.098);
	if (rock_id < 3.5) return vec3(0.250, 0.240, 0.232);
	if (rock_id < 4.5) return vec3(0.175, 0.170, 0.158);
	if (rock_id < 5.5) return vec3(0.365, 0.230, 0.140);
	if (rock_id < 6.5) return vec3(0.155, 0.146, 0.132);
	if (rock_id < 7.5) return vec3(0.415, 0.400, 0.352);
	if (rock_id < 8.5) return vec3(0.375, 0.358, 0.318);
	if (rock_id < 9.5) return vec3(0.265, 0.222, 0.180);
	if (rock_id < 10.5) return vec3(0.455, 0.442, 0.422);
	if (rock_id < 11.5) return vec3(0.330, 0.298, 0.260);
	return vec3(0.120, 0.145, 0.118);
}

void vertex() {
	float rock_id = floor(INSTANCE_CUSTOM.r * 12.0 + 0.5);
	float erodibility = INSTANCE_CUSTOM.g;
	float moisture = INSTANCE_CUSTOM.b;
	float jitter = INSTANCE_CUSTOM.a;
	vec3 base = rock_color(rock_id);
	float colour_jitter = mix(0.78, 1.17, jitter);
	if (u_stone_kind == 0) {
		v_stone_color = clamp(base * colour_jitter + vec3(0.012), vec3(0.015), vec3(0.62));
		v_stone_roughness = mix(0.72, 0.92, erodibility);
	} else {
		vec3 water_worn = mix(base, vec3(0.255, 0.245, 0.225), 0.38);
		v_stone_color = clamp(water_worn * colour_jitter, vec3(0.02), vec3(0.60));
		v_stone_roughness = mix(0.60, 0.82, 1.0 - moisture);
	}
}

void fragment() {
	ALBEDO = v_stone_color;
	ROUGHNESS = v_stone_roughness;
	SPECULAR = 0.18;
	METALLIC = 0.0;
}
