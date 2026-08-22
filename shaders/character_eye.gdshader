shader_type spatial;
render_mode diffuse_burley, specular_schlick_ggx;

uniform vec4 iris_color : source_color = vec4(0.16, 0.31, 0.42, 1.0);
uniform vec3 left_iris_center = vec3(-0.029879, 1.619379, 0.143274);
uniform vec3 right_iris_center = vec3(0.029879, 1.619379, 0.143274);
uniform float iris_radius : hint_range(0.003, 0.010) = 0.0064;
uniform float pupil_size : hint_range(0.15, 0.70) = 0.34;
uniform float limbal_strength : hint_range(0.0, 1.0) = 0.72;
uniform float sclera_roughness : hint_range(0.05, 0.65) = 0.28;

varying vec3 local_position;

float hash11(float p) {
	return fract(sin(p * 127.1) * 43758.5453123);
}

float hash21(vec2 p) {
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

void vertex() {
	local_position = VERTEX;
}

void fragment() {
	vec3 center = local_position.x < 0.0 ? left_iris_center : right_iris_center;
	vec2 iris_uv = (local_position.xy - center.xy) / iris_radius;
	float radius = length(iris_uv);
	float angle = atan(iris_uv.y, iris_uv.x);
	float iris_mask = 1.0 - smoothstep(0.91, 1.03, radius);
	float pupil_mask = 1.0 - smoothstep(pupil_size - 0.025, pupil_size + 0.035, radius);
	float limbal = smoothstep(0.70, 0.98, radius) * iris_mask;

	float angular_fiber = hash11(floor((angle + 3.14159265) * 42.0));
	float fine_fiber = sin(angle * 93.0 + radius * 31.0) * 0.5 + 0.5;
	float radial_breakup = hash21(vec2(floor(angle * 31.0), floor(radius * 22.0)));
	float fiber = mix(angular_fiber, fine_fiber, 0.55) * mix(0.72, 1.18, radial_breakup);
	vec3 iris_dark = iris_color.rgb * 0.24;
	vec3 iris_light = min(iris_color.rgb * 1.65 + vec3(0.035, 0.025, 0.005), vec3(1.0));
	vec3 iris = mix(iris_dark, iris_light, clamp(fiber * (1.0 - radius * 0.34), 0.0, 1.0));
	iris = mix(iris, iris_dark * 0.42, limbal * limbal_strength);
	iris = mix(iris, vec3(0.004, 0.006, 0.008), pupil_mask);

	float sclera_noise = hash21(floor(local_position.xy * 2200.0));
	vec3 sclera = vec3(0.93, 0.945, 0.925);
	sclera += (sclera_noise - 0.5) * vec3(0.018, 0.012, 0.010);
	float side_redness = smoothstep(0.010, 0.035, abs(local_position.x - center.x));
	sclera = mix(sclera, vec3(0.93, 0.82, 0.80), side_redness * 0.20);

	ALBEDO = mix(sclera, iris, iris_mask);
	METALLIC = 0.0;
	SPECULAR = 0.86;
	ROUGHNESS = mix(sclera_roughness, 0.18, iris_mask);
	CLEARCOAT = 1.0;
	CLEARCOAT_ROUGHNESS = 0.035;
	EMISSION = iris * iris_mask * 0.012;
}
