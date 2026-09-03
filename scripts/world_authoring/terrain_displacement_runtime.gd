class_name TerrainDisplacementRuntime
extends Node
## Shared scalar displacement runtime for Planet Studio terrain graphs.
##
## Graphs compile to a compact register bytecode once. The same bytecode is:
## - uploaded as a tiny RGBA32F texture for the terrain vertex shader; and
## - evaluated here for contact/physics queries.
##
## This deliberately supports only operations with deterministic scalar semantics
## on both CPU and GPU. Texture/triplanar displacement is rejected with a warning
## until a matching contact-side sampler exists; visual-only terrain is not allowed.

# Keep GPU interpreter storage small enough to remain resident per terrain vertex.
# Larger local arrays have triggered Vulkan device loss on current NVIDIA drivers.
const MAX_INSTRUCTIONS: int = 32
const BIOME_COUNT: int = 18

# Per-biome terrain profiles (the "Biome Terrain" sub-tab) are deliberately kept
# out of the displacement bytecode VM. Enabling one used to compile a program,
# which made the renderer swap in the authored-terrain shader variant -- a large
# surface-shader recompile that triggers Vulkan device loss on current NVIDIA
# drivers. Instead these slots lower to a handful of uniforms consumed by
# shaders/terrain_biome_profile.gdshaderinc (always compiled, no local arrays) and
# are mirrored here for contact/AGL queries. The math matches OP_NOISE_LAYER /
# OP_RIDGED_MOUNTAINS / OP_EROSION_CHANNELS / OP_SEDIMENT_DEPOSIT / OP_TERRACE_RELIEF.
const BIOME_PROFILE_SLOT_PREFIX := "simple-biome-terrain-"
const BIOME_PROFILE_MAX: int = 6
const BIOME_PROFILE_EROSION_SEED_OFFSET: int = 7919
const BIOME_PROFILE_SEDIMENT_SEED_OFFSET: int = 104729

# Keep these opcodes in lock-step with terrain_author_displacement_bytecode.gdshaderinc.
const OP_CONST := 0
const OP_INPUT_BASE_HEIGHT := 1
const OP_INPUT_BIOME := 2
const OP_INPUT_LEVEL := 3
const OP_INPUT_TIME := 4
const OP_NOISE := 5
const OP_ADD := 6
const OP_SUB := 7
const OP_MUL := 8
const OP_DIV := 9
const OP_MIN := 10
const OP_MAX := 11
const OP_ABS := 12
const OP_POWER := 13
const OP_CLAMP := 14
const OP_SMOOTHSTEP := 15
const OP_REMAP := 16
const OP_MIX := 17
const OP_SCALE := 18
const OP_LEVEL_MASK := 19
const OP_BIOME_MASK := 20
const OP_REPLACE := 21
const OP_NOISE_LAYER := 22
const OP_RIDGED_MOUNTAINS := 23
const OP_EROSION_CHANNELS := 24
const OP_SEDIMENT_DEPOSIT := 25
## Kept after the existing generic terrain operations so saved bytecode remains
## compatible.  The fourth parameter is the pattern seed; z is terrace count.
const OP_TERRACE_RELIEF := 29

var _headers := PackedVector4Array()
var _params := PackedVector4Array()
var _code_texture: ImageTexture
var _output_index: int = -1
var _active: bool = false
var _warnings: PackedStringArray = PackedStringArray()
var _fingerprint: String = ""
var _compile_generation: int = 0
var _biome_preview: Node
# Parsed per-biome terrain profiles, newest compile. Each entry:
#   {biome_id, base_type (0 fbm / 1 ridged / 2 terraced), scale, amount,
#    base_param (passes 1-4 or terrace steps 2-24), seed,
#    erosion, erosion_scale, sedimentation, sediment_scale, strength}
var _biome_profiles: Array = []


func _ready() -> void:
	add_to_group(&"terrain_displacement_runtime")


func clear() -> void:
	_headers = PackedVector4Array()
	_params = PackedVector4Array()
	_output_index = -1
	_active = false
	_warnings = PackedStringArray()
	_fingerprint = ""
	_code_texture = null
	_compile_generation += 1
	_biome_profiles = []


func profile_fingerprint(terrain: Resource) -> String:
	if terrain == null:
		return "none"
	var parts: PackedStringArray = PackedStringArray()
	var slots_value: Variant = terrain.get(&"displacement_slots")
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
		var ids_value: Variant = slot.get(&"biome_ids")
		if ids_value is PackedInt32Array:
			parts.append(str(ids_value))
		parts.append(str(int(slot.get(&"biome_mask_mode"))))
	return "|".join(parts)


func compile_from_terrain(terrain: Resource) -> Dictionary:
	_headers = PackedVector4Array()
	_params = PackedVector4Array()
	_output_index = -1
	_active = false
	_warnings = PackedStringArray()
	_fingerprint = profile_fingerprint(terrain)
	_compile_generation += 1
	_biome_profiles = []
	if terrain == null:
		_publish_texture()
		return stats()

	var slots_value: Variant = terrain.get(&"displacement_slots")
	if not (slots_value is Array):
		_publish_texture()
		return stats()

	var accumulator: int = -1
	var slot_index: int = 0
	for slot_value: Variant in slots_value as Array:
		var slot: Resource = slot_value as Resource
		if slot == null or not bool(slot.get(&"enabled")) or int(slot.get(&"domain")) != 0:
			slot_index += 1
			continue
		if String(slot.get(&"slot_id")).begins_with(BIOME_PROFILE_SLOT_PREFIX):
			# Biome Terrain profiles never enter the VM (see BIOME_PROFILE_* notes).
			if _biome_profiles.size() < BIOME_PROFILE_MAX:
				var parsed: Dictionary = _parse_biome_profile_slot(slot)
				if not parsed.is_empty():
					_biome_profiles.append(parsed)
			slot_index += 1
			continue
		var graph: Resource = slot.get(&"graph") as Resource
		if graph == null:
			_warnings.append("%s has no displacement graph." % String(slot.get(&"display_name")))
			slot_index += 1
			continue
		var value_index: int = _compile_graph(graph, slot_index)
		if value_index < 0:
			slot_index += 1
			continue

		# Clipmap and biome masks are part of the authoritative expression, so the
		# CPU contact evaluator sees exactly the same inclusion rules as the GPU.
		# Masks/scale that are the identity are skipped so slots that apply to every
		# ring at unit strength (the common biome-profile case) stay within the
		# 32-instruction VM budget.
		var level_mask: int = int(slot.get(&"clipmap_level_mask"))
		if (level_mask & 0x7fff) != 0x7fff:
			value_index = _append_instruction(OP_LEVEL_MASK, value_index, -1, -1,
				Vector4(float(level_mask), 0.0, 0.0, 0.0))
		var biome_mode: int = int(slot.get(&"biome_mask_mode"))
		if biome_mode != 0:
			var biome_mask: int = 0
			var biome_ids_value: Variant = slot.get(&"biome_ids")
			if biome_ids_value is PackedInt32Array:
				for biome_id: int in biome_ids_value as PackedInt32Array:
					if biome_id >= 0 and biome_id < BIOME_COUNT:
						biome_mask |= 1 << biome_id
			value_index = _append_instruction(OP_BIOME_MASK, value_index, -1, -1,
				Vector4(float(biome_mode), float(biome_mask), 0.0, 0.0))
		if not is_equal_approx(float(slot.get(&"strength")), 1.0):
			value_index = _append_instruction(OP_SCALE, value_index, -1, -1,
				Vector4(float(slot.get(&"strength")), 0.0, 0.0, 0.0))
		if value_index < 0:
			break

		var blend_mode: int = int(slot.get(&"blend_mode"))
		if accumulator < 0:
			if blend_mode == 1: # SUBTRACT
				accumulator = _append_instruction(OP_SCALE, value_index, -1, -1,
					Vector4(-1.0, 0.0, 0.0, 0.0))
			else:
				accumulator = value_index
		else:
			var blend_op: int = OP_ADD
			match blend_mode:
				1: blend_op = OP_SUB
				2: blend_op = OP_MUL
				3: blend_op = OP_MIN
				4: blend_op = OP_MAX
				5: blend_op = OP_REPLACE
				_: blend_op = OP_ADD
			accumulator = _append_instruction(blend_op, accumulator, value_index)
		if accumulator < 0:
			break
		slot_index += 1

	_output_index = accumulator
	_active = _output_index >= 0 and not _headers.is_empty()
	_publish_texture()
	return stats()


func _compile_graph(graph: Resource, graph_seed_index: int) -> int:
	var nodes_value: Variant = graph.get(&"nodes")
	var links_value: Variant = graph.get(&"links")
	if not (nodes_value is Array) or not (links_value is Array):
		_warnings.append("Malformed displacement graph arrays.")
		return -1
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
		if String(node.get("type", "")) == "OUTPUT_DISPLACEMENT":
			output_id = node_id
	if output_id.is_empty():
		_warnings.append("Displacement graph has no OUTPUT_DISPLACEMENT node.")
		return -1

	var input_sources: Dictionary = {}
	for link_value: Variant in links_value as Array:
		if not (link_value is Dictionary):
			continue
		var link: Dictionary = link_value as Dictionary
		var to_id: String = String(link.get("to", ""))
		var to_port: int = int(link.get("to_port", 0))
		var from_id: String = String(link.get("from", ""))
		if not to_id.is_empty() and not from_id.is_empty():
			input_sources["%s:%d" % [to_id, to_port]] = from_id
	var output_source: String = String(input_sources.get("%s:0" % output_id, ""))
	if output_source.is_empty():
		# An unconnected output is a valid zero-displacement graph.
		return _append_instruction(OP_CONST, -1, -1, -1, Vector4.ZERO)

	var memo: Dictionary = {}
	var visiting: Dictionary = {}
	var graph_seed: int = abs(String(graph.get(&"graph_id")).hash() ^ (graph_seed_index * 97531)) & 0x7fffffff
	return _compile_node(output_source, nodes_by_id, input_sources, memo, visiting, graph_seed)


func _compile_node(node_id: String, nodes: Dictionary, inputs: Dictionary,
		memo: Dictionary, visiting: Dictionary, graph_seed: int) -> int:
	if memo.has(node_id):
		return int(memo[node_id])
	if visiting.has(node_id):
		_warnings.append("Cycle detected at displacement node %s." % node_id)
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
			result = _append_instruction(OP_CONST, -1, -1, -1,
				Vector4(float(parameters.get("value", 0.0)), 0.0, 0.0, 0.0))
		"CONSTANT_COLOR":
			var color := Color(parameters.get("value", Color.WHITE))
			result = _append_instruction(OP_CONST, -1, -1, -1,
				Vector4((color.r + color.g + color.b) / 3.0, 0.0, 0.0, 0.0))
		"GAME_INPUT":
			var source: String = String(parameters.get("source", "terrain_height_m"))
			match source:
				"terrain_height_m": result = _append_instruction(OP_INPUT_BASE_HEIGHT)
				"biome_id": result = _append_instruction(OP_INPUT_BIOME)
				"clipmap_level": result = _append_instruction(OP_INPUT_LEVEL)
				"time_s": result = _append_instruction(OP_INPUT_TIME)
				_:
					_warnings.append("GAME_INPUT '%s' is not scalar-authoritative for displacement yet; using 0." % source)
					result = _append_instruction(OP_CONST, -1, -1, -1, Vector4.ZERO)
		"NOISE":
			result = _append_instruction(OP_NOISE, -1, -1, -1,
				Vector4(maxf(absf(float(parameters.get("scale", 1.0))), 0.000001),
					float(int(parameters.get("seed", graph_seed)) & 0x7fffffff), 0.0, 0.0))
		"NOISE_LAYER", "RIDGED_MOUNTAINS", "EROSION_CHANNELS", "SEDIMENT_DEPOSIT", "TERRACE_RELIEF":
			var terrain_input: int = _compile_input(node_id, 0, nodes, inputs,
				memo, visiting, graph_seed)
			var operation: int = OP_NOISE_LAYER
			match node_type:
				"RIDGED_MOUNTAINS": operation = OP_RIDGED_MOUNTAINS
				"EROSION_CHANNELS": operation = OP_EROSION_CHANNELS
				"SEDIMENT_DEPOSIT": operation = OP_SEDIMENT_DEPOSIT
				"TERRACE_RELIEF": operation = OP_TERRACE_RELIEF
			var default_amount: float = 100.0 if node_type == "NOISE_LAYER" else 250.0
			if node_type == "EROSION_CHANNELS":
				default_amount = 40.0
			elif node_type == "SEDIMENT_DEPOSIT":
				default_amount = 25.0
			elif node_type == "TERRACE_RELIEF":
				default_amount = 80.0
			var detail_count: int = clampi(int(parameters.get("passes", 3)), 1, 4)
			if node_type == "TERRACE_RELIEF":
				detail_count = clampi(int(parameters.get("steps", 6)), 2, 24)
			result = _append_instruction(operation, terrain_input, -1, -1, Vector4(
				maxf(absf(float(parameters.get("scale", 6.0))), 0.000001),
				float(parameters.get("amount", default_amount)),
				float(detail_count),
				float(int(parameters.get("seed", graph_seed)) & 0x000fffff)))
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
				Vector4(float(parameters.get("out_min", 0.0)), float(parameters.get("out_max", 1.0)), 0.0, 0.0))
		"MIX":
			result = _append_instruction(OP_MIX,
				_compile_input(node_id, 0, nodes, inputs, memo, visiting, graph_seed),
				_compile_input(node_id, 1, nodes, inputs, memo, visiting, graph_seed),
				_compile_input(node_id, 2, nodes, inputs, memo, visiting, graph_seed))
		"TEXTURE_2D", "TRIPLANAR":
			_warnings.append("%s displacement is disabled until the contact query has the same texture sampler." % node_type)
			result = _append_instruction(OP_CONST, -1, -1, -1, Vector4.ZERO)
		_:
			_warnings.append("Unsupported scalar displacement node '%s'; using 0." % node_type)
			result = _append_instruction(OP_CONST, -1, -1, -1, Vector4.ZERO)

	visiting.erase(node_id)
	memo[node_id] = result
	return result


func _compile_input(node_id: String, port: int, nodes: Dictionary, inputs: Dictionary,
		memo: Dictionary, visiting: Dictionary, graph_seed: int) -> int:
	var source_id: String = String(inputs.get("%s:%d" % [node_id, port], ""))
	if source_id.is_empty():
		return _append_instruction(OP_CONST, -1, -1, -1, Vector4.ZERO)
	return _compile_node(source_id, nodes, inputs, memo, visiting, graph_seed)


func _append_instruction(op: int, a: int = -1, b: int = -1, c: int = -1,
		parameters: Vector4 = Vector4.ZERO) -> int:
	if _headers.size() >= MAX_INSTRUCTIONS:
		if not _warnings.has("Displacement program exceeded %d instructions." % MAX_INSTRUCTIONS):
			_warnings.append("Displacement program exceeded %d instructions." % MAX_INSTRUCTIONS)
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
	material.set_shader_parameter("u_author_disp_code", _code_texture)
	material.set_shader_parameter("u_author_disp_count", _headers.size())
	material.set_shader_parameter("u_author_disp_output", _output_index)
	material.set_shader_parameter("u_author_disp_ready", 1.0 if _active else 0.0)


func evaluate_height(direction: Vector3, base_height_m: float = 0.0,
		clipmap_level: int = 0, biome_id: int = -1, time_s: float = NAN) -> float:
	if direction.length_squared() <= 1e-12:
		return 0.0
	var d: Vector3 = direction.normalized()
	var resolved_biome: int = biome_id
	if resolved_biome < 0:
		resolved_biome = _sample_authored_biome(d)
	# Biome Terrain profiles compose on top of the VM program (or stand alone when
	# no other displacement slot is enabled and the VM is inactive).
	var biome_term: float = _biome_profiles_height(d, resolved_biome)
	if not _active or _output_index < 0:
		return biome_term
	var resolved_time: float = time_s
	if is_nan(resolved_time):
		resolved_time = fmod(float(Time.get_ticks_msec()) * 0.001, 3600.0)
	var values := PackedFloat32Array()
	values.resize(_headers.size())
	for index: int in _headers.size():
		var h: Vector4 = _headers[index]
		var p: Vector4 = _params[index]
		var op: int = int(round(h.x))
		var a: float = _reg(values, int(round(h.y)), index)
		var b: float = _reg(values, int(round(h.z)), index)
		var c: float = _reg(values, int(round(h.w)), index)
		var out: float = 0.0
		match op:
			OP_CONST: out = p.x
			OP_INPUT_BASE_HEIGHT: out = base_height_m
			OP_INPUT_BIOME: out = float(resolved_biome)
			OP_INPUT_LEVEL: out = float(clipmap_level)
			OP_INPUT_TIME: out = resolved_time
			OP_NOISE: out = _value_noise_3d(d * p.x, int(round(p.y)))
			OP_ADD: out = a + b
			OP_SUB: out = a - b
			OP_MUL: out = a * b
			OP_DIV: out = a / b if absf(b) > 1e-12 else 0.0
			OP_MIN: out = minf(a, b)
			OP_MAX: out = maxf(a, b)
			OP_ABS: out = absf(a)
			OP_POWER: out = signf(a) * pow(absf(a), b) if absf(a) > 1e-12 else 0.0
			OP_CLAMP: out = clampf(a, minf(b, c), maxf(b, c))
			OP_SMOOTHSTEP: out = _smoothstep(b, c, a)
			OP_REMAP:
				var t: float = (a - b) / (c - b) if absf(c - b) > 1e-12 else 0.0
				out = lerpf(p.x, p.y, t)
			OP_MIX: out = lerpf(a, b, clampf(c, 0.0, 1.0))
			OP_SCALE: out = a * p.x
			OP_LEVEL_MASK:
				var level_mask: int = int(round(p.x))
				var level: int = clampi(clipmap_level, 0, 30)
				out = a if (level_mask & (1 << level)) != 0 else 0.0
			OP_BIOME_MASK:
				var mode: int = int(round(p.x))
				var mask: int = int(round(p.y))
				var bid: int = clampi(resolved_biome, 0, BIOME_COUNT - 1)
				var selected: bool = (mask & (1 << bid)) != 0
				var allow: bool = true
				if mode == 1:
					allow = selected
				elif mode == 2:
					allow = not selected
				out = a if allow else 0.0
			OP_REPLACE: out = b
			OP_NOISE_LAYER:
				out = a + _terrain_fbm(d, p.x, int(round(p.w)), int(round(p.z))) * p.y
			OP_RIDGED_MOUNTAINS:
				var ridge: float = 1.0 - absf(_terrain_fbm(
					d, p.x, int(round(p.w)), int(round(p.z))))
				out = a + ridge * ridge * p.y
			OP_EROSION_CHANNELS:
				var erosion_field: float = _terrain_fbm(
					d, p.x, int(round(p.w)), int(round(p.z)))
				var channel: float = clampf(1.0 - absf(erosion_field), 0.0, 1.0)
				out = a - channel * channel * channel * p.y
			OP_SEDIMENT_DEPOSIT:
				var sediment_field: float = _terrain_fbm(
					d, p.x, int(round(p.w)), int(round(p.z)))
				var deposit: float = clampf(1.0 - absf(sediment_field * 1.65), 0.0, 1.0)
				out = a + deposit * deposit * p.y
			OP_TERRACE_RELIEF:
				var terrace_field: float = _terrain_fbm(d, p.x, int(round(p.w)), 1)
				var terrace_steps: float = float(clampi(int(round(p.z)), 2, 24))
				var terrace: float = floor((terrace_field * 0.5 + 0.5) * terrace_steps) \
					/ maxf(terrace_steps - 1.0, 1.0)
				out = a + (terrace * 2.0 - 1.0) * p.y
			_: out = 0.0
		values[index] = out if is_finite(out) else 0.0
	var vm_result: float = float(values[_output_index]) if _output_index < values.size() else 0.0
	return vm_result + biome_term


func _reg(values: PackedFloat32Array, register_index: int, current: int) -> float:
	if register_index < 0 or register_index >= current or register_index >= values.size():
		return 0.0
	return float(values[register_index])


func _smoothstep(edge0: float, edge1: float, value: float) -> float:
	if absf(edge1 - edge0) <= 1e-12:
		return 0.0
	var t: float = clampf((value - edge0) / (edge1 - edge0), 0.0, 1.0)
	return t * t * (3.0 - 2.0 * t)


func _value_noise_3d(position: Vector3, seed: int) -> float:
	var ix: int = floori(position.x)
	var iy: int = floori(position.y)
	var iz: int = floori(position.z)
	var fx: float = position.x - float(ix)
	var fy: float = position.y - float(iy)
	var fz: float = position.z - float(iz)
	fx = fx * fx * (3.0 - 2.0 * fx)
	fy = fy * fy * (3.0 - 2.0 * fy)
	fz = fz * fz * (3.0 - 2.0 * fz)
	var c000 := _noise_hash(ix, iy, iz, seed)
	var c100 := _noise_hash(ix + 1, iy, iz, seed)
	var c010 := _noise_hash(ix, iy + 1, iz, seed)
	var c110 := _noise_hash(ix + 1, iy + 1, iz, seed)
	var c001 := _noise_hash(ix, iy, iz + 1, seed)
	var c101 := _noise_hash(ix + 1, iy, iz + 1, seed)
	var c011 := _noise_hash(ix, iy + 1, iz + 1, seed)
	var c111 := _noise_hash(ix + 1, iy + 1, iz + 1, seed)
	var x00 := lerpf(c000, c100, fx)
	var x10 := lerpf(c010, c110, fx)
	var x01 := lerpf(c001, c101, fx)
	var x11 := lerpf(c011, c111, fx)
	return lerpf(lerpf(x00, x10, fy), lerpf(x01, x11, fy), fz)


func _terrain_fbm(direction: Vector3, scale: float, seed: int, passes: int) -> float:
	var total: float = 0.0
	var normalizer: float = 0.0
	var amplitude: float = 1.0
	var frequency: float = maxf(absf(scale), 0.000001)
	for octave: int in clampi(passes, 1, 4):
		var sample: float = _value_noise_3d(direction * frequency, seed + octave * 1013)
		total += sample * amplitude
		normalizer += amplitude
		frequency *= 2.0
		amplitude *= 0.5
	return total / normalizer if normalizer > 0.0 else 0.0


func _noise_hash(x: int, y: int, z: int, seed: int) -> float:
	var h: int = ((x & 0xffffffff) * 374761393 \
		+ (y & 0xffffffff) * 668265263 \
		+ (z & 0xffffffff) * 2147483647 \
		+ (seed & 0xffffffff) * 1274126177) & 0xffffffff
	h = ((h ^ (h >> 13)) * 1274126177) & 0xffffffff
	h = (h ^ (h >> 16)) & 0xffffffff
	return float(h & 0x00ffffff) / 16777215.0 * 2.0 - 1.0


func _parse_biome_profile_slot(slot: Resource) -> Dictionary:
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return {}
	var nodes_value: Variant = graph.get(&"nodes")
	if not (nodes_value is Array):
		return {}
	var biome_id: int = 0
	var ids_value: Variant = slot.get(&"biome_ids")
	if ids_value is PackedInt32Array and (ids_value as PackedInt32Array).size() > 0:
		biome_id = clampi(int((ids_value as PackedInt32Array)[0]), 0, BIOME_COUNT - 1)
	var profile: Dictionary = {
		"biome_id": biome_id,
		"base_type": 0,
		"scale": 6.0,
		"amount": 0.0,
		"base_param": 3.0,
		"seed": 1337,
		"erosion": 0.0,
		"erosion_scale": 8.0,
		"sedimentation": 0.0,
		"sediment_scale": 3.0,
		"strength": float(slot.get(&"strength")),
	}
	var found_base: bool = false
	for value: Variant in nodes_value as Array:
		if not (value is Dictionary):
			continue
		var node: Dictionary = value as Dictionary
		var node_type: String = String(node.get("type", ""))
		var parameters: Dictionary = node.get("parameters", {}) as Dictionary
		match node_type:
			"NOISE_LAYER", "BILLOW_NOISE":
				profile["base_type"] = 0
				profile["scale"] = float(parameters.get("scale", profile["scale"]))
				profile["amount"] = float(parameters.get("amount", profile["amount"]))
				profile["base_param"] = float(clampi(int(parameters.get("passes", 3)), 1, 4))
				profile["seed"] = int(parameters.get("seed", profile["seed"]))
				found_base = true
			"RIDGED_MOUNTAINS", "VORONOI_RIDGES":
				profile["base_type"] = 1
				profile["scale"] = float(parameters.get("scale", profile["scale"]))
				profile["amount"] = float(parameters.get("amount", profile["amount"]))
				profile["base_param"] = float(clampi(int(parameters.get("passes", 3)), 1, 4))
				profile["seed"] = int(parameters.get("seed", profile["seed"]))
				found_base = true
			"TERRACE_RELIEF":
				profile["base_type"] = 2
				profile["scale"] = float(parameters.get("scale", profile["scale"]))
				profile["amount"] = float(parameters.get("amount", profile["amount"]))
				profile["base_param"] = float(clampi(int(parameters.get("steps", 6)), 2, 24))
				profile["seed"] = int(parameters.get("seed", profile["seed"]))
				found_base = true
			"EROSION_CHANNELS":
				profile["erosion"] = maxf(0.0, float(parameters.get("amount", 0.0)))
				profile["erosion_scale"] = float(parameters.get("scale", profile["erosion_scale"]))
			"SEDIMENT_DEPOSIT":
				profile["sedimentation"] = maxf(0.0, float(parameters.get("amount", 0.0)))
				profile["sediment_scale"] = float(parameters.get("scale", profile["sediment_scale"]))
	if not found_base and profile["erosion"] <= 0.0 and profile["sedimentation"] <= 0.0:
		return {}
	return profile


# Metres this profile set adds at `direction` for `biome_id`. Mirrors
# shaders/terrain_biome_profile.gdshaderinc exactly so contact/AGL matches render.
func _biome_profiles_height(direction: Vector3, biome_id: int) -> float:
	if _biome_profiles.is_empty():
		return 0.0
	var d: Vector3 = direction.normalized()
	var here: int = clampi(biome_id, 0, BIOME_COUNT - 1)
	var total: float = 0.0
	for entry_value: Variant in _biome_profiles:
		var entry: Dictionary = entry_value as Dictionary
		if int(entry.get("biome_id", -1)) != here:
			continue
		var seed: int = int(entry.get("seed", 1337))
		var scale: float = float(entry.get("scale", 6.0))
		var amount: float = float(entry.get("amount", 0.0))
		var base_param: int = int(round(float(entry.get("base_param", 3.0))))
		var base_type: int = int(entry.get("base_type", 0))
		var h: float = 0.0
		if base_type == 1:
			var ridge: float = 1.0 - absf(_terrain_fbm(d, scale, seed, base_param))
			h = ridge * ridge * amount
		elif base_type == 2:
			var field: float = _terrain_fbm(d, scale, seed, 1)
			var steps: float = float(clampi(base_param, 2, 24))
			var terrace: float = floor((field * 0.5 + 0.5) * steps) / maxf(steps - 1.0, 1.0)
			h = (terrace * 2.0 - 1.0) * amount
		else:
			h = _terrain_fbm(d, scale, seed, base_param) * amount
		var erosion: float = float(entry.get("erosion", 0.0))
		if erosion > 0.0:
			var e_field: float = _terrain_fbm(d, float(entry.get("erosion_scale", 8.0)),
				seed + BIOME_PROFILE_EROSION_SEED_OFFSET, 3)
			var channel: float = clampf(1.0 - absf(e_field), 0.0, 1.0)
			h -= channel * channel * channel * erosion
		var sedimentation: float = float(entry.get("sedimentation", 0.0))
		if sedimentation > 0.0:
			var s_field: float = _terrain_fbm(d, float(entry.get("sediment_scale", 3.0)),
				seed + BIOME_PROFILE_SEDIMENT_SEED_OFFSET, 3)
			var deposit: float = clampf(1.0 - absf(s_field * 1.65), 0.0, 1.0)
			h += deposit * deposit * sedimentation
		total += h * float(entry.get("strength", 1.0))
	return total


# {count, a, b, c} packed for shaders/terrain_biome_profile.gdshaderinc.
func biome_profile_uniforms() -> Dictionary:
	var count: int = mini(_biome_profiles.size(), BIOME_PROFILE_MAX)
	var a := PackedVector4Array()
	var b := PackedVector4Array()
	var c := PackedVector4Array()
	a.resize(BIOME_PROFILE_MAX)
	b.resize(BIOME_PROFILE_MAX)
	c.resize(BIOME_PROFILE_MAX)
	for i: int in count:
		var entry: Dictionary = _biome_profiles[i] as Dictionary
		a[i] = Vector4(float(int(entry.get("biome_id", 0))), float(int(entry.get("base_type", 0))),
			float(entry.get("scale", 6.0)), float(entry.get("amount", 0.0)))
		b[i] = Vector4(float(entry.get("base_param", 3.0)), float(int(entry.get("seed", 1337))),
			float(entry.get("erosion", 0.0)), float(entry.get("erosion_scale", 8.0)))
		c[i] = Vector4(float(entry.get("sedimentation", 0.0)), float(entry.get("sediment_scale", 3.0)),
			float(entry.get("strength", 1.0)), 0.0)
	return {"count": count, "a": a, "b": b, "c": c}


func biome_profile_count() -> int:
	return mini(_biome_profiles.size(), BIOME_PROFILE_MAX)


func _sample_authored_biome(direction: Vector3) -> int:
	if _biome_preview == null or not is_instance_valid(_biome_preview):
		_biome_preview = get_tree().root.find_child("PlanetStudioBiomePreview", true, false)
	if _biome_preview == null:
		return 0
	var center_valid: Variant = _biome_preview.get("_center_valid")
	var has_content: Variant = _biome_preview.get("_has_content")
	var image_value: Variant = _biome_preview.get("_image")
	if not bool(center_valid) or not bool(has_content) or not (image_value is Image):
		return 0
	var center: Vector3 = _biome_preview.get("_center_dir") as Vector3
	var right: Vector3 = _biome_preview.get("_center_right") as Vector3
	var up: Vector3 = _biome_preview.get("_center_up") as Vector3
	var denom: float = direction.dot(center)
	if denom <= 0.01:
		return 0
	var radius: float = maxf(float(Planet.cfg.planet_radius), 1.0) if Planet.cfg != null else 1.0
	var image: Image = image_value as Image
	var stats_value: Variant = _biome_preview.call("stats") if _biome_preview.has_method("stats") else {}
	var spacing_m: float = 2.0
	if stats_value is Dictionary:
		spacing_m = maxf(float((stats_value as Dictionary).get("spacing_m", 2.0)), 0.001)
	var half_pixel: float = (float(image.get_width()) - 1.0) * 0.5
	var projection_scale: float = radius / denom
	var plane_x: float = direction.dot(right) * projection_scale
	var plane_y: float = direction.dot(up) * projection_scale
	var px: int = roundi(plane_x / spacing_m + half_pixel)
	var py: int = roundi(half_pixel - plane_y / spacing_m)
	if px < 0 or py < 0 or px >= image.get_width() or py >= image.get_height():
		return 0
	var sample: Color = image.get_pixel(px, py)
	if sample.g < 0.5:
		return 0
	return clampi(roundi(sample.r * 19.0), 0, BIOME_COUNT - 1)


func stats() -> Dictionary:
	return {
		"active": _active,
		"instructions": _headers.size(),
		"max_instructions": MAX_INSTRUCTIONS,
		"output_index": _output_index,
		"warnings": _warnings.duplicate(),
		"fingerprint": _fingerprint,
		"generation": _compile_generation,
		"gpu_texture_ready": _code_texture != null,
		"cpu_gpu_shared_bytecode": true,
		"biome_profiles": biome_profile_count(),
	}
