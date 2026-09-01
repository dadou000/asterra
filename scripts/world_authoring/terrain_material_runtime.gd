class_name TerrainMaterialRuntime
extends Node
## GPU material-graph compiler for Planet Studio terrain authoring.
##
## Material graphs compile to a compact vec4 register program. The production
## terrain shader evaluates the program after its normal generated/PBR material,
## so authored graphs can use the current renderer output as `base_*` inputs and
## selectively replace or compose albedo, normal, roughness, metallic and AO.
##
## Slot level/biome masks are compiled into the same program. Global slots run for
## every biome on their selected L levels; biome slots are additional overrides.

# This becomes a per-fragment vec4 register file; a large historical limit caused
# driver timeouts even for otherwise modest graphs.
const MAX_INSTRUCTIONS: int = 32
const MAX_TEXTURES: int = 8
const BIOME_COUNT: int = 18

# Keep these opcodes in lock-step with terrain_author_material_bytecode.gdshaderinc.
const OP_CONST := 0
const OP_INPUT_BASE_ALBEDO := 1
const OP_INPUT_BASE_NORMAL := 2
const OP_INPUT_BASE_ROUGHNESS := 3
const OP_INPUT_BASE_METALLIC := 4
const OP_INPUT_BASE_AO := 5
const OP_INPUT_HEIGHT := 6
const OP_INPUT_BIOME := 7
const OP_INPUT_LEVEL := 8
const OP_INPUT_TIME := 9
const OP_INPUT_SLOPE := 10
const OP_INPUT_DIRECTION := 11
const OP_INPUT_WORLD_POSITION := 12
const OP_INPUT_SURFACE_NORMAL := 13
const OP_INPUT_CAMERA_DISTANCE := 14
const OP_NOISE := 15
const OP_ADD := 16
const OP_SUB := 17
const OP_MUL := 18
const OP_DIV := 19
const OP_MIN := 20
const OP_MAX := 21
const OP_ABS := 22
const OP_POWER := 23
const OP_CLAMP := 24
const OP_SMOOTHSTEP := 25
const OP_REMAP := 26
const OP_MIX := 27
const OP_TEXTURE_EQUIRECT := 28
const OP_TRIPLANAR := 29
const OP_NORMAL_BLEND := 30
const OP_SCOPE_MASK := 31
const OP_SLOT_BLEND := 32

var _headers := PackedVector4Array()
var _params := PackedVector4Array()
var _code_texture: ImageTexture
var _output_albedo: int = -1
var _output_normal: int = -1
var _output_roughness: int = -1
var _output_metallic: int = -1
var _output_ao: int = -1
var _active: bool = false
var _warnings: PackedStringArray = PackedStringArray()
var _fingerprint: String = ""
var _compile_generation: int = 0
var _texture_paths: PackedStringArray = PackedStringArray()
var _textures: Array[Texture2D] = []
var _compiled_slot_outputs: int = 0


func _ready() -> void:
	add_to_group(&"terrain_material_runtime")


func clear() -> void:
	_headers = PackedVector4Array()
	_params = PackedVector4Array()
	_code_texture = null
	_output_albedo = -1
	_output_normal = -1
	_output_roughness = -1
	_output_metallic = -1
	_output_ao = -1
	_active = false
	_warnings = PackedStringArray()
	_fingerprint = ""
	_texture_paths = PackedStringArray()
	_textures.clear()
	_compiled_slot_outputs = 0
	_compile_generation += 1


func profile_fingerprint(terrain: Resource) -> String:
	if terrain == null:
		return "none"
	var parts := PackedStringArray()
	var slots_value: Variant = terrain.get(&"material_slots")
	if not (slots_value is Array):
		return "empty"
	for slot_value: Variant in slots_value as Array:
		var slot: Resource = slot_value as Resource
		if slot == null:
			continue
		var graph: Resource = slot.get(&"graph") as Resource
		parts.append("%s:%s:%s:%s:%s:%s" % [
			String(slot.get(&"slot_id")),
			str(bool(slot.get(&"enabled"))),
			str(int(slot.get(&"blend_mode"))),
			str(float(slot.get(&"strength"))),
			str(int(slot.get(&"clipmap_level_mask"))),
			str(int(graph.get(&"revision")) if graph != null else -1),
		])
		parts.append(str(int(slot.get(&"biome_mask_mode"))))
		var ids_value: Variant = slot.get(&"biome_ids")
		if ids_value is PackedInt32Array:
			parts.append(str(ids_value))
	return "|".join(parts)


func compile_from_terrain(terrain: Resource) -> Dictionary:
	_headers = PackedVector4Array()
	_params = PackedVector4Array()
	_code_texture = null
	_output_albedo = -1
	_output_normal = -1
	_output_roughness = -1
	_output_metallic = -1
	_output_ao = -1
	_active = false
	_warnings = PackedStringArray()
	_texture_paths = PackedStringArray()
	_textures.clear()
	_compiled_slot_outputs = 0
	_fingerprint = profile_fingerprint(terrain)
	_compile_generation += 1

	if terrain == null:
		_publish_texture()
		return stats()
	var slots_value: Variant = terrain.get(&"material_slots")
	if not (slots_value is Array):
		_publish_texture()
		return stats()

	# Accumulators start at the actual production material. An authored material
	# graph can therefore be a small override instead of a duplicate of the large
	# generated terrain PBR shader.
	var accum: Array[int] = [
		_append_instruction(OP_INPUT_BASE_ALBEDO),
		_append_instruction(OP_INPUT_BASE_NORMAL),
		_append_instruction(OP_INPUT_BASE_ROUGHNESS),
		_append_instruction(OP_INPUT_BASE_METALLIC),
		_append_instruction(OP_INPUT_BASE_AO),
	]

	var slot_index: int = 0
	for slot_value: Variant in slots_value as Array:
		var slot: Resource = slot_value as Resource
		if slot == null or not bool(slot.get(&"enabled")) or int(slot.get(&"domain")) != 1:
			slot_index += 1
			continue
		var graph: Resource = slot.get(&"graph") as Resource
		if graph == null:
			_warnings.append("%s has no material graph." % String(slot.get(&"display_name")))
			slot_index += 1
			continue

		var outputs: Array[int] = _compile_graph(graph, slot_index)
		if outputs.size() != 5:
			slot_index += 1
			continue
		var biome_mask: int = _slot_biome_mask(slot)
		var scope_index: int = _append_instruction(OP_SCOPE_MASK, -1, -1, -1,
			Vector4(float(int(slot.get(&"clipmap_level_mask"))),
				float(int(slot.get(&"biome_mask_mode"))), float(biome_mask), 0.0))
		if scope_index < 0:
			break

		for channel: int in 5:
			var candidate: int = outputs[channel]
			if candidate < 0:
				continue
			var blended: int = _append_instruction(OP_SLOT_BLEND,
				accum[channel], candidate, scope_index,
				Vector4(float(int(slot.get(&"blend_mode"))),
					float(slot.get(&"strength")), float(channel), 0.0))
			if blended < 0:
				break
			accum[channel] = blended
			_compiled_slot_outputs += 1
		slot_index += 1
		if _headers.size() >= MAX_INSTRUCTIONS:
			break

	_output_albedo = accum[0]
	_output_normal = accum[1]
	_output_roughness = accum[2]
	_output_metallic = accum[3]
	_output_ao = accum[4]
	_active = _compiled_slot_outputs > 0 and not _headers.is_empty()
	_publish_texture()
	return stats()


func _compile_graph(graph: Resource, graph_seed_index: int) -> Array[int]:
	var empty: Array[int] = [-1, -1, -1, -1, -1]
	var nodes_value: Variant = graph.get(&"nodes")
	var links_value: Variant = graph.get(&"links")
	if not (nodes_value is Array) or not (links_value is Array):
		_warnings.append("Malformed material graph arrays.")
		return empty

	var nodes_by_id: Dictionary = {}
	var output_id: String = ""
	for node_value: Variant in nodes_value as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		var node_id: String = String(node.get("id", ""))
		if node_id.is_empty():
			continue
		nodes_by_id[node_id] = node
		if String(node.get("type", "")) == "OUTPUT_MATERIAL":
			output_id = node_id
	if output_id.is_empty():
		_warnings.append("Material graph has no OUTPUT_MATERIAL node.")
		return empty

	var input_sources: Dictionary = {}
	for link_value: Variant in links_value as Array:
		if not (link_value is Dictionary):
			continue
		var link: Dictionary = link_value as Dictionary
		var to_id: String = String(link.get("to", ""))
		var from_id: String = String(link.get("from", ""))
		if to_id.is_empty() or from_id.is_empty():
			continue
		input_sources["%s:%d" % [to_id, int(link.get("to_port", 0))]] = from_id

	var memo: Dictionary = {}
	var visiting: Dictionary = {}
	var graph_seed: int = abs(String(graph.get(&"graph_id")).hash() ^
		(graph_seed_index * 97531)) & 0x7fffffff
	var outputs: Array[int] = []
	for port: int in 5:
		var source_id: String = String(input_sources.get("%s:%d" % [output_id, port], ""))
		if source_id.is_empty():
			outputs.append(-1)
		else:
			outputs.append(_compile_node(source_id, nodes_by_id, input_sources,
				memo, visiting, graph_seed))
	return outputs


func _compile_node(node_id: String, nodes: Dictionary, inputs: Dictionary,
		memo: Dictionary, visiting: Dictionary, graph_seed: int) -> int:
	if memo.has(node_id):
		return int(memo[node_id])
	if visiting.has(node_id):
		_warnings.append("Cycle detected at material node %s." % node_id)
		return _append_instruction(OP_CONST, -1, -1, -1, Vector4.ZERO)
	if not nodes.has(node_id):
		return _append_instruction(OP_CONST, -1, -1, -1, Vector4.ZERO)

	visiting[node_id] = true
	var node: Dictionary = nodes[node_id] as Dictionary
	var node_type: String = String(node.get("type", "CONSTANT_FLOAT"))
	var parameters: Dictionary = node.get("parameters", {}) as Dictionary
	var result: int = -1

	match node_type:
		"CONSTANT_FLOAT":
			var value: float = float(parameters.get("value", 0.0))
			result = _append_instruction(OP_CONST, -1, -1, -1,
				Vector4(value, value, value, value))
		"CONSTANT_COLOR":
			var color := Color(parameters.get("value", Color.WHITE))
			result = _append_instruction(OP_CONST, -1, -1, -1,
				Vector4(color.r, color.g, color.b, color.a))
		"GAME_INPUT":
			result = _compile_game_input(String(parameters.get("source", "base_albedo")))
		"NOISE":
			result = _append_instruction(OP_NOISE, -1, -1, -1,
				Vector4(maxf(absf(float(parameters.get("scale", 1.0))), 0.000001),
					float(int(parameters.get("seed", graph_seed)) & 0x7fffffff), 0.0, 0.0))
		"TEXTURE_2D":
			var texture_index: int = _texture_index(String(parameters.get("asset_id", "")))
			if texture_index < 0:
				result = _append_instruction(OP_CONST, -1, -1, -1, Vector4.ONE)
			else:
				result = _append_instruction(OP_TEXTURE_EQUIRECT, -1, -1, -1,
					Vector4(float(texture_index),
						maxf(absf(float(parameters.get("scale", 1.0))), 0.000001), 0.0, 0.0))
		"TRIPLANAR":
			var texture_source: String = String(inputs.get("%s:0" % node_id, ""))
			var texture_index: int = _texture_index_from_node(texture_source, nodes)
			var scale_index: int = _compile_input_or_const(node_id, 1, nodes, inputs,
				memo, visiting, graph_seed, 1.0)
			if texture_index < 0:
				result = _append_instruction(OP_CONST, -1, -1, -1, Vector4.ONE)
			else:
				result = _append_instruction(OP_TRIPLANAR, scale_index, -1, -1,
					Vector4(float(texture_index), 0.0, 0.0, 0.0))
		"ABS":
			result = _append_instruction(OP_ABS,
				_compile_input(node_id, 0, nodes, inputs, memo, visiting, graph_seed))
		"ADD", "SUBTRACT", "MULTIPLY", "DIVIDE", "MIN", "MAX", "POWER":
			var a: int = _compile_input(node_id, 0, nodes, inputs, memo, visiting, graph_seed)
			var b: int = _compile_input(node_id, 1, nodes, inputs, memo, visiting, graph_seed)
			var op: int = OP_ADD
			match node_type:
				"SUBTRACT": op = OP_SUB
				"MULTIPLY": op = OP_MUL
				"DIVIDE": op = OP_DIV
				"MIN": op = OP_MIN
				"MAX": op = OP_MAX
				"POWER": op = OP_POWER
			result = _append_instruction(op, a, b)
		"CLAMP":
			result = _append_instruction(OP_CLAMP,
				_compile_input(node_id, 0, nodes, inputs, memo, visiting, graph_seed),
				_compile_input(node_id, 1, nodes, inputs, memo, visiting, graph_seed),
				_compile_input(node_id, 2, nodes, inputs, memo, visiting, graph_seed))
		"SMOOTHSTEP":
			result = _append_instruction(OP_SMOOTHSTEP,
				_compile_input(node_id, 0, nodes, inputs, memo, visiting, graph_seed),
				_compile_input(node_id, 1, nodes, inputs, memo, visiting, graph_seed),
				_compile_input(node_id, 2, nodes, inputs, memo, visiting, graph_seed))
		"REMAP":
			result = _append_instruction(OP_REMAP,
				_compile_input(node_id, 0, nodes, inputs, memo, visiting, graph_seed),
				_compile_input(node_id, 1, nodes, inputs, memo, visiting, graph_seed),
				_compile_input(node_id, 2, nodes, inputs, memo, visiting, graph_seed),
				Vector4(float(parameters.get("out_min", 0.0)),
					float(parameters.get("out_max", 1.0)), 0.0, 0.0))
		"MIX":
			result = _append_instruction(OP_MIX,
				_compile_input(node_id, 0, nodes, inputs, memo, visiting, graph_seed),
				_compile_input(node_id, 1, nodes, inputs, memo, visiting, graph_seed),
				_compile_input(node_id, 2, nodes, inputs, memo, visiting, graph_seed))
		"NORMAL_BLEND":
			result = _append_instruction(OP_NORMAL_BLEND,
				_compile_input(node_id, 0, nodes, inputs, memo, visiting, graph_seed),
				_compile_input(node_id, 1, nodes, inputs, memo, visiting, graph_seed))
		_:
			_warnings.append("Unsupported material node '%s'; using 0." % node_type)
			result = _append_instruction(OP_CONST, -1, -1, -1, Vector4.ZERO)

	visiting.erase(node_id)
	memo[node_id] = result
	return result


func _compile_game_input(source: String) -> int:
	match source:
		"base_albedo": return _append_instruction(OP_INPUT_BASE_ALBEDO)
		"base_normal": return _append_instruction(OP_INPUT_BASE_NORMAL)
		"base_roughness": return _append_instruction(OP_INPUT_BASE_ROUGHNESS)
		"base_metallic": return _append_instruction(OP_INPUT_BASE_METALLIC)
		"base_ao": return _append_instruction(OP_INPUT_BASE_AO)
		"terrain_height_m": return _append_instruction(OP_INPUT_HEIGHT)
		"biome_id": return _append_instruction(OP_INPUT_BIOME)
		"clipmap_level": return _append_instruction(OP_INPUT_LEVEL)
		"time_s": return _append_instruction(OP_INPUT_TIME)
		"slope": return _append_instruction(OP_INPUT_SLOPE)
		"planet_direction": return _append_instruction(OP_INPUT_DIRECTION)
		"world_position": return _append_instruction(OP_INPUT_WORLD_POSITION)
		"surface_normal": return _append_instruction(OP_INPUT_SURFACE_NORMAL)
		"camera_distance_m": return _append_instruction(OP_INPUT_CAMERA_DISTANCE)
		_:
			_warnings.append("GAME_INPUT '%s' is not available to the live material compiler yet; using 0." % source)
			return _append_instruction(OP_CONST, -1, -1, -1, Vector4.ZERO)


func _compile_input(node_id: String, port: int, nodes: Dictionary, inputs: Dictionary,
		memo: Dictionary, visiting: Dictionary, graph_seed: int) -> int:
	var source_id: String = String(inputs.get("%s:%d" % [node_id, port], ""))
	if source_id.is_empty():
		return _append_instruction(OP_CONST, -1, -1, -1, Vector4.ZERO)
	return _compile_node(source_id, nodes, inputs, memo, visiting, graph_seed)


func _compile_input_or_const(node_id: String, port: int, nodes: Dictionary,
		inputs: Dictionary, memo: Dictionary, visiting: Dictionary, graph_seed: int,
		default_value: float) -> int:
	var source_id: String = String(inputs.get("%s:%d" % [node_id, port], ""))
	if source_id.is_empty():
		return _append_instruction(OP_CONST, -1, -1, -1,
			Vector4(default_value, default_value, default_value, default_value))
	return _compile_node(source_id, nodes, inputs, memo, visiting, graph_seed)


func _texture_index_from_node(node_id: String, nodes: Dictionary) -> int:
	if node_id.is_empty() or not nodes.has(node_id):
		_warnings.append("TRIPLANAR expects a TEXTURE_2D node on its Texture input.")
		return -1
	var node: Dictionary = nodes[node_id] as Dictionary
	if String(node.get("type", "")) != "TEXTURE_2D":
		_warnings.append("TRIPLANAR Texture input must connect directly to TEXTURE_2D.")
		return -1
	var parameters: Dictionary = node.get("parameters", {}) as Dictionary
	return _texture_index(String(parameters.get("asset_id", "")))


func _texture_index(path: String) -> int:
	var clean: String = path.strip_edges()
	if clean.is_empty():
		_warnings.append("Material texture node has no resource path.")
		return -1
	var existing: int = _texture_paths.find(clean)
	if existing >= 0:
		return existing
	if _textures.size() >= MAX_TEXTURES:
		_warnings.append("Material graph exceeded %d unique textures." % MAX_TEXTURES)
		return -1
	if not ResourceLoader.exists(clean):
		_warnings.append("Material texture does not exist: %s" % clean)
		return -1
	var resource: Resource = load(clean)
	if not (resource is Texture2D):
		_warnings.append("Material texture is not Texture2D: %s" % clean)
		return -1
	_texture_paths.append(clean)
	_textures.append(resource as Texture2D)
	return _textures.size() - 1


func _slot_biome_mask(slot: Resource) -> int:
	var mask: int = 0
	var ids_value: Variant = slot.get(&"biome_ids")
	if ids_value is PackedInt32Array:
		for biome_id: int in ids_value as PackedInt32Array:
			if biome_id >= 0 and biome_id < BIOME_COUNT:
				mask |= 1 << biome_id
	return mask


func _append_instruction(op: int, a: int = -1, b: int = -1, c: int = -1,
		parameters: Vector4 = Vector4.ZERO) -> int:
	if _headers.size() >= MAX_INSTRUCTIONS:
		var message := "Material program exceeded %d instructions." % MAX_INSTRUCTIONS
		if not _warnings.has(message):
			_warnings.append(message)
		return -1
	var index: int = _headers.size()
	_headers.append(Vector4(float(op), float(a), float(b), float(c)))
	_params.append(parameters)
	return index


func _publish_texture() -> void:
	var rows: int = maxi(1, _headers.size())
	var image := Image.create(2, rows, false, Image.FORMAT_RGBAF)
	image.fill(Color(0.0, 0.0, 0.0, 0.0))
	for index: int in _headers.size():
		var h: Vector4 = _headers[index]
		var p: Vector4 = _params[index]
		image.set_pixel(0, index, Color(h.x, h.y, h.z, h.w))
		image.set_pixel(1, index, Color(p.x, p.y, p.z, p.w))
	_code_texture = ImageTexture.create_from_image(image)


func bind_material(material: ShaderMaterial) -> void:
	if material == null:
		return
	material.set_shader_parameter("u_author_mat_code", _code_texture)
	material.set_shader_parameter("u_author_mat_count", _headers.size())
	material.set_shader_parameter("u_author_mat_output_albedo", _output_albedo)
	material.set_shader_parameter("u_author_mat_output_normal", _output_normal)
	material.set_shader_parameter("u_author_mat_output_roughness", _output_roughness)
	material.set_shader_parameter("u_author_mat_output_metallic", _output_metallic)
	material.set_shader_parameter("u_author_mat_output_ao", _output_ao)
	material.set_shader_parameter("u_author_mat_ready", 1.0 if _active else 0.0)
	var ready_mask: int = 0
	for index: int in MAX_TEXTURES:
		var texture: Texture2D = _textures[index] if index < _textures.size() else null
		material.set_shader_parameter("u_author_mat_tex%d" % index, texture)
		if texture != null:
			ready_mask |= 1 << index
	material.set_shader_parameter("u_author_mat_texture_ready_mask", ready_mask)


func stats() -> Dictionary:
	return {
		"active": _active,
		"instructions": _headers.size(),
		"max_instructions": MAX_INSTRUCTIONS,
		"compiled_slot_outputs": _compiled_slot_outputs,
		"textures": _texture_paths.duplicate(),
		"max_textures": MAX_TEXTURES,
		"warnings": _warnings.duplicate(),
		"fingerprint": _fingerprint,
		"generation": _compile_generation,
		"gpu_texture_ready": _code_texture != null,
	}
