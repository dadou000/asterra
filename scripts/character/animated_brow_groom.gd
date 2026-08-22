extends "res://scripts/character/natural_brow_groom.gd"

## Runtime facial-animation bridge for procedural brows.
##
## The imported face uses ARKit-style blend shapes, while the procedural brow
## geometry is generated separately and therefore cannot inherit those GPU
## morphs automatically. This layer reads the five brow-related blend-shape
## channels and deforms every brow LOD in its vertex shader. No procedural mesh
## rebuild is needed while the face animates.

const BROW_EXPRESSION_POLL_INTERVAL := 1.0 / 60.0
const BROW_EXPRESSION_EPSILON := 0.0005

var _animated_brow_material: ShaderMaterial
var _expression_elapsed := 0.0
var _brow_channels: Dictionary = {
	"inner_up": [],
	"down_left": [],
	"down_right": [],
	"outer_up_left": [],
	"outer_up_right": []
}
var _last_expression := Vector4(-1.0, -1.0, -1.0, -1.0)
var _last_outer_right := -1.0
var _mapped_channel_count := 0

func configure(character: Node3D, meshes: Array[MeshInstance3D], character_bottom: float, character_height: float, front_sign: float = 1.0, rebuild_initial: bool = true) -> bool:
	var ok: bool = super.configure(character, meshes, character_bottom, character_height, front_sign, rebuild_initial)
	if not ok:
		return false
	_discover_brow_channels()
	_sync_brow_layout_uniforms()
	_update_brow_expression(true)
	return true

func _create_render_nodes() -> void:
	super._create_render_nodes()
	_animated_brow_material = _make_animated_brow_material()
	# natural_brow_groom already owns this variable. Replace its static far-LOD
	# shader with the animated variant while retaining the soft alpha falloff.
	_lod2_fade_material = _make_animated_lod2_material()

func _apply_material_settings() -> void:
	super._apply_material_settings()
	if _animated_brow_material != null:
		var brow_color: Color = brow_settings.get("color", Color("3a281e"))
		_animated_brow_material.set_shader_parameter("brow_color", brow_color)
	_sync_brow_layout_uniforms()

func rebuild_brows() -> void:
	super.rebuild_brows()
	if _brow_instance != null and _animated_brow_material != null:
		_brow_instance.material_override = _animated_brow_material
	if _brow_lod1_instance != null and _animated_brow_material != null:
		_brow_lod1_instance.material_override = _animated_brow_material
	if _brow_lod2_instance != null and _lod2_fade_material != null:
		_brow_lod2_instance.material_override = _lod2_fade_material
	_sync_brow_layout_uniforms()
	_update_brow_expression(true)

func _process(delta: float) -> void:
	super._process(delta)
	_expression_elapsed += delta
	if _expression_elapsed < BROW_EXPRESSION_POLL_INTERVAL:
		return
	_expression_elapsed = 0.0
	_update_brow_expression(false)

func _discover_brow_channels() -> void:
	_brow_channels["inner_up"] = []
	_brow_channels["down_left"] = []
	_brow_channels["down_right"] = []
	_brow_channels["outer_up_left"] = []
	_brow_channels["outer_up_right"] = []
	_mapped_channel_count = 0

	for mesh_instance in _source_meshes:
		if mesh_instance == null or mesh_instance.mesh == null:
			continue
		for index in mesh_instance.mesh.get_blend_shape_count():
			var raw_name: String = str(mesh_instance.mesh.get_blend_shape_name(index))
			var normalized: String = _normalize_shape_name(raw_name)
			var channel := ""

			if normalized.contains("browinnerup"):
				channel = "inner_up"
			elif normalized.contains("browdownleft") or normalized.contains("browdownl"):
				channel = "down_left"
			elif normalized.contains("browdownright") or normalized.contains("browdownr"):
				channel = "down_right"
			elif normalized.contains("browouterupleft") or normalized.contains("browouterupl"):
				channel = "outer_up_left"
			elif normalized.contains("browouterupright") or normalized.contains("browouterupr"):
				channel = "outer_up_right"

			if channel.is_empty():
				continue
			var entries: Array = _brow_channels[channel]
			entries.append({
				"mesh": mesh_instance,
				"index": index,
				"name": raw_name
			})
			_brow_channels[channel] = entries

	for channel_name in _brow_channels.keys():
		var entries: Array = _brow_channels[channel_name]
		if not entries.is_empty():
			_mapped_channel_count += 1

func _normalize_shape_name(value: String) -> String:
	return value.to_lower().replace("_", "").replace("-", "").replace(".", "").replace(" ", "")

func _read_channel(channel: String) -> float:
	var entries: Array = _brow_channels.get(channel, [])
	var result := 0.0
	for entry_variant in entries:
		var entry: Dictionary = entry_variant
		var mesh_instance: MeshInstance3D = entry.get("mesh") as MeshInstance3D
		if mesh_instance == null or mesh_instance.mesh == null:
			continue
		var index := int(entry.get("index", -1))
		if index < 0 or index >= mesh_instance.mesh.get_blend_shape_count():
			continue
		result = maxf(result, clampf(mesh_instance.get_blend_shape_value(index), 0.0, 1.0))
	return result

func _update_brow_expression(force: bool) -> void:
	if _animated_brow_material == null or _lod2_fade_material == null:
		return

	var inner_up := _read_channel("inner_up")
	var down_left := _read_channel("down_left")
	var down_right := _read_channel("down_right")
	var outer_left := _read_channel("outer_up_left")
	var outer_right := _read_channel("outer_up_right")
	var expression := Vector4(inner_up, down_left, down_right, outer_left)

	if not force:
		var largest_change := 0.0
		largest_change = maxf(largest_change, absf(expression.x - _last_expression.x))
		largest_change = maxf(largest_change, absf(expression.y - _last_expression.y))
		largest_change = maxf(largest_change, absf(expression.z - _last_expression.z))
		largest_change = maxf(largest_change, absf(expression.w - _last_expression.w))
		largest_change = maxf(largest_change, absf(outer_right - _last_outer_right))
		if largest_change < BROW_EXPRESSION_EPSILON:
			return

	_last_expression = expression
	_last_outer_right = outer_right
	_set_expression_uniform("brow_inner_up", inner_up)
	_set_expression_uniform("brow_down_left", down_left)
	_set_expression_uniform("brow_down_right", down_right)
	_set_expression_uniform("brow_outer_up_left", outer_left)
	_set_expression_uniform("brow_outer_up_right", outer_right)

func _set_expression_uniform(parameter: StringName, value: float) -> void:
	if _animated_brow_material != null:
		_animated_brow_material.set_shader_parameter(parameter, value)
	if _lod2_fade_material != null:
		_lod2_fade_material.set_shader_parameter(parameter, value)

func _sync_brow_layout_uniforms() -> void:
	if _mount == null or _head.is_empty():
		return
	if _animated_brow_material == null or _lod2_fade_material == null:
		return

	var center_world: Vector3 = _head["center"]
	var character_basis: Basis = _character.global_transform.basis.orthonormalized() if _character != null else Basis.IDENTITY
	var up_world: Vector3 = (character_basis * Vector3.UP).normalized()
	var front_world: Vector3 = (character_basis * Vector3(0.0, 0.0, _front_sign)).normalized()
	var right_world: Vector3 = up_world.cross(front_world).normalized()
	var inverse_mount_basis: Basis = _mount.global_transform.basis.inverse()
	var center_local: Vector3 = _mount.to_local(center_world)
	var right_local: Vector3 = (inverse_mount_basis * right_world).normalized()
	var up_local: Vector3 = (inverse_mount_basis * up_world).normalized()
	var front_local: Vector3 = (inverse_mount_basis * front_world).normalized()

	var rx := float(_head["rx"])
	var middle_spacing := clampf(float(brow_settings.get("middle_spacing", 0.008)), -0.020, 0.060)
	var brow_width := clampf(float(brow_settings.get("width", 0.73)), 0.55, 1.45)
	var inner_world := clampf(middle_spacing * 0.5, -rx * 0.22, rx * 0.62)
	var outer_world := maxf(rx * 0.79 * brow_width, inner_world + rx * 0.16)

	# Convert the world-space layout distances to mount-local units. This keeps
	# the shader correct if the character scene is scaled later.
	var inner_point_local: Vector3 = _mount.to_local(center_world + right_world * inner_world)
	var outer_point_local: Vector3 = _mount.to_local(center_world + right_world * outer_world)
	var inner_local := absf((inner_point_local - center_local).dot(right_local))
	var outer_local := absf((outer_point_local - center_local).dot(right_local))
	if outer_local <= inner_local + 0.0001:
		outer_local = inner_local + 0.0001

	for material in [_animated_brow_material, _lod2_fade_material]:
		var shader_material: ShaderMaterial = material
		shader_material.set_shader_parameter("brow_center_local", center_local)
		shader_material.set_shader_parameter("brow_right_local", right_local)
		shader_material.set_shader_parameter("brow_up_local", up_local)
		shader_material.set_shader_parameter("brow_front_local", front_local)
		shader_material.set_shader_parameter("brow_inner_distance", inner_local)
		shader_material.set_shader_parameter("brow_outer_distance", outer_local)
		shader_material.set_shader_parameter("character_height", _character_height)

func _make_animated_brow_material() -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = """
shader_type spatial;
render_mode cull_disabled, diffuse_burley, specular_schlick_ggx;

uniform vec4 brow_color : source_color = vec4(0.23, 0.16, 0.12, 1.0);
uniform vec3 brow_center_local = vec3(0.0);
uniform vec3 brow_right_local = vec3(1.0, 0.0, 0.0);
uniform vec3 brow_up_local = vec3(0.0, 1.0, 0.0);
uniform vec3 brow_front_local = vec3(0.0, 0.0, 1.0);
uniform float brow_inner_distance = 0.004;
uniform float brow_outer_distance = 0.060;
uniform float character_height = 1.75;
uniform float brow_inner_up = 0.0;
uniform float brow_down_left = 0.0;
uniform float brow_down_right = 0.0;
uniform float brow_outer_up_left = 0.0;
uniform float brow_outer_up_right = 0.0;

void vertex() {
	float lateral = dot(VERTEX - brow_center_local, brow_right_local);
	float t = clamp((abs(lateral) - brow_inner_distance) / max(brow_outer_distance - brow_inner_distance, 0.0001), 0.0, 1.0);
	float character_right = step(0.0, lateral);
	float outer_up = mix(brow_outer_up_left, brow_outer_up_right, character_right);
	float brow_down = mix(brow_down_left, brow_down_right, character_right);
	float inner_mask = 1.0 - smoothstep(0.10, 0.62, t);
	float outer_mask = smoothstep(0.26, 0.96, t);
	float down_mask = 1.0 - 0.16 * outer_mask;

	float vertical_offset = 0.0;
	vertical_offset += brow_inner_up * inner_mask * character_height * 0.0050;
	vertical_offset += outer_up * outer_mask * character_height * 0.0046;
	vertical_offset -= brow_down * down_mask * character_height * 0.0043;

	// Forehead skin comes slightly forward as it raises and recedes slightly
	// when pulled down. This is intentionally much smaller than vertical motion.
	float front_offset = vertical_offset * 0.075;
	VERTEX += brow_up_local * vertical_offset + brow_front_local * front_offset;
}

void fragment() {
	ALBEDO = brow_color.rgb;
	ROUGHNESS = 0.56;
	METALLIC = 0.0;
	SPECULAR = 0.32;
}
"""
	var material := ShaderMaterial.new()
	material.shader = shader
	material.set_shader_parameter("brow_color", brow_settings.get("color", Color("3a281e")))
	return material

func _make_animated_lod2_material() -> ShaderMaterial:
	var shader := Shader.new()
	shader.code = """
shader_type spatial;
render_mode blend_mix, cull_disabled, diffuse_burley, specular_schlick_ggx;

uniform vec4 brow_color : source_color = vec4(0.23, 0.16, 0.12, 1.0);
uniform float opacity = 0.82;
uniform float inner_fade = 0.14;
uniform float outer_fade = 0.18;
uniform float edge_softness = 0.22;
uniform vec3 brow_center_local = vec3(0.0);
uniform vec3 brow_right_local = vec3(1.0, 0.0, 0.0);
uniform vec3 brow_up_local = vec3(0.0, 1.0, 0.0);
uniform vec3 brow_front_local = vec3(0.0, 0.0, 1.0);
uniform float brow_inner_distance = 0.004;
uniform float brow_outer_distance = 0.060;
uniform float character_height = 1.75;
uniform float brow_inner_up = 0.0;
uniform float brow_down_left = 0.0;
uniform float brow_down_right = 0.0;
uniform float brow_outer_up_left = 0.0;
uniform float brow_outer_up_right = 0.0;

void vertex() {
	float lateral = dot(VERTEX - brow_center_local, brow_right_local);
	float t = clamp((abs(lateral) - brow_inner_distance) / max(brow_outer_distance - brow_inner_distance, 0.0001), 0.0, 1.0);
	float character_right = step(0.0, lateral);
	float outer_up = mix(brow_outer_up_left, brow_outer_up_right, character_right);
	float brow_down = mix(brow_down_left, brow_down_right, character_right);
	float inner_mask = 1.0 - smoothstep(0.10, 0.62, t);
	float outer_mask = smoothstep(0.26, 0.96, t);
	float down_mask = 1.0 - 0.16 * outer_mask;
	float vertical_offset = brow_inner_up * inner_mask * character_height * 0.0050;
	vertical_offset += outer_up * outer_mask * character_height * 0.0046;
	vertical_offset -= brow_down * down_mask * character_height * 0.0043;
	VERTEX += brow_up_local * vertical_offset + brow_front_local * (vertical_offset * 0.075);
}

void fragment() {
	float vertical_edge = smoothstep(0.0, max(edge_softness, 0.001), UV.x) * smoothstep(0.0, max(edge_softness, 0.001), 1.0 - UV.x);
	float inner_alpha = inner_fade <= 0.0001 ? 1.0 : smoothstep(0.0, inner_fade, UV.y);
	float outer_alpha = outer_fade <= 0.0001 ? 1.0 : smoothstep(0.0, outer_fade, 1.0 - UV.y);
	ALBEDO = brow_color.rgb;
	ROUGHNESS = 0.58;
	SPECULAR = 0.28;
	ALPHA = opacity * vertical_edge * inner_alpha * outer_alpha;
}
"""
	var material := ShaderMaterial.new()
	material.shader = shader
	return material

func diagnostics() -> String:
	var base: String = super.diagnostics()
	return "%s • brow facial animation %d/5 channels" % [base, _mapped_channel_count]
