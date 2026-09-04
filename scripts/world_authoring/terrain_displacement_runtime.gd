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

# Per-biome terrain (the "Biome Terrain" sub-tab) is deliberately kept out of the
# displacement bytecode VM. Enabling one used to compile a program, which made the
# renderer swap in the authored-terrain shader variant -- a large surface-shader
# recompile that triggers Vulkan device loss on current NVIDIA drivers. Instead a
# biome slot lowers to a stack of layer uniforms + a per-biome blend width,
# consumed by shaders/terrain_biome_profile.gdshaderinc (always compiled, no local
# arrays) and mirrored here for contact/AGL queries. The per-layer math matches
# OP_NOISE_LAYER / OP_RIDGED_MOUNTAINS / OP_EROSION_CHANNELS / OP_SEDIMENT_DEPOSIT
# / OP_TERRACE_RELIEF.
const BIOME_PROFILE_SLOT_PREFIX := "simple-biome-terrain-"
# layer_type: 0 fbm noise, 1 ridged, 2 terraced, 3 water erosion, 4 sediment.
const BIOME_LAYER_MAX: int = 16

# Biome Texture (surface look) mirrors Biome Terrain's own VM-avoidance --
# see terrain_biome_texture.gdshaderinc's header. One node type ("TEXTURE_BAND")
# covers every layer; its parameters (height/slope range, colour, texture
# choice, noise) differentiate them, since unlike height layers there is no
# separate "shape" per layer type.
const BIOME_TEXTURE_SLOT_PREFIX := "simple-biome-texture-"
const BIOME_TEX_LAYER_MAX: int = 16
# Mirrors terrain_biome_profile.gdshaderinc's BP_MAX_FREQUENCY. The CPU's
# double precision never needs this for its own accuracy, but
# _biome_erosion_channels/_biome_sediment_deposit derive secondary
# frequencies (a "flow"/slope frequency) from the layer's base frequency, and
# an unclamped base frequency would put those secondary frequencies at
# different values here than on the (clamped) GPU side at extreme settings.
const BIOME_MAX_FREQUENCY: float = 1500000.0
const BIOME_LAYER_TYPE_BY_NODE: Dictionary = {
	"NOISE_LAYER": 0, "BILLOW_NOISE": 0,
	"RIDGED_MOUNTAINS": 1, "VORONOI_RIDGES": 1,
	"TERRACE_RELIEF": 2,
	"EROSION_CHANNELS": 3,
	"SEDIMENT_DEPOSIT": 4,
}
# Output-node parameter that carries the km-scale biome boundary softness.
const BIOME_BLEND_PARAM := "biome_blend_km"

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
# Parsed per-biome terrain layers, newest compile, packed grouped by biome_id.
# Each entry: {biome_id, layer_type, scale, amount, param, seed}
#   layer_type: 0 fbm noise, 1 ridged, 2 terraced, 3 water erosion, 4 sediment.
#   param: fbm passes (1-4) or terrace steps (2-24). amount already folds in the
#   slot blend strength and may be negative to carve.
var _biome_profiles: Array = []
# Per-biome boundary softness in metres (index = biome id, 0 = hard edge).
var _biome_blend_m: PackedFloat32Array = _zeroed_blend()
# Parsed per-biome surface-look (texture) layers, newest compile -- see
# terrain_biome_texture.gdshaderinc's packing comment for the field meanings.
# Purely visual: no CPU-side evaluation function, since surface look doesn't
# feed contact/AGL the way _biome_profiles' height does.
var _biome_textures: Array = []


static func _zeroed_blend() -> PackedFloat32Array:
	var out := PackedFloat32Array()
	out.resize(BIOME_COUNT)
	return out


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
	_biome_blend_m = _zeroed_blend()
	_biome_textures = []


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
	_biome_blend_m = _zeroed_blend()
	_biome_textures = []
	if terrain == null:
		_publish_texture()
		return stats()

	var slots_value: Variant = terrain.get(&"displacement_slots")
	if not (slots_value is Array):
		_publish_texture()
		return stats()

	var accumulator: int = -1
	var slot_index: int = 0
	var biome_layers_dropped: int = 0
	var biome_texture_layers_dropped: int = 0
	for slot_value: Variant in slots_value as Array:
		var slot: Resource = slot_value as Resource
		if slot == null or int(slot.get(&"domain")) != 0:
			slot_index += 1
			continue
		if String(slot.get(&"slot_id")).begins_with(BIOME_PROFILE_SLOT_PREFIX):
			# Biome Terrain never enters the VM (see BIOME_LAYER_* notes), and
			# deliberately ignores `enabled` here: that field is a legacy
			# editor-only "only the biome you're currently viewing in Planet
			# Studio is live" gate (see world_authoring_editor_live_phase46.gd's
			# _phase47_migrate_biome_slots) that left customised biomes
			# invisible -- in the editor's own preview and in the actual game
			# -- until you happened to revisit that exact biome's tab. An
			# empty/unauthored biome slot already contributes zero layers on
			# its own, and BIOME_LAYER_MAX below already caps the real
			# constraint (the fixed-size GPU uniform array), so there is
			# nothing left for `enabled` to usefully gate.
			var parsed: Dictionary = _parse_biome_layer_slot(slot)
			var parsed_biome: int = int(parsed.get("biome_id", -1))
			if parsed_biome >= 0 and parsed_biome < BIOME_COUNT:
				_biome_blend_m[parsed_biome] = float(parsed.get("blend_m", 0.0))
				for layer_value: Variant in parsed.get("layers", []) as Array:
					if _biome_profiles.size() < BIOME_LAYER_MAX:
						_biome_profiles.append(layer_value)
					else:
						biome_layers_dropped += 1
			slot_index += 1
			continue
		if String(slot.get(&"slot_id")).begins_with(BIOME_TEXTURE_SLOT_PREFIX):
			# Biome Texture: same reasoning as Biome Terrain just above --
			# never the VM, `enabled` not checked for the same reason.
			var parsed_tex: Array = _parse_biome_texture_slot(slot)
			for layer_value: Variant in parsed_tex:
				if _biome_textures.size() < BIOME_TEX_LAYER_MAX:
					_biome_textures.append(layer_value)
				else:
					biome_texture_layers_dropped += 1
			slot_index += 1
			continue
		if not bool(slot.get(&"enabled")):
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
	if biome_layers_dropped > 0:
		_warnings.append(
			"%d biome terrain layer(s) did not fit in the %d-layer budget shared across all biomes and are not rendering. Remove a layer from a less-important biome to fit within it."
			% [biome_layers_dropped, BIOME_LAYER_MAX])
	if biome_texture_layers_dropped > 0:
		_warnings.append(
			"%d biome texture layer(s) did not fit in the %d-layer budget shared across all biomes and are not rendering. Remove a layer from a less-important biome to fit within it."
			% [biome_texture_layers_dropped, BIOME_TEX_LAYER_MAX])
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


func _parse_biome_layer_slot(slot: Resource) -> Dictionary:
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return {}
	var nodes_value: Variant = graph.get(&"nodes")
	var links_value: Variant = graph.get(&"links")
	if not (nodes_value is Array) or not (links_value is Array):
		return {}
	var biome_id: int = 0
	var ids_value: Variant = slot.get(&"biome_ids")
	if ids_value is PackedInt32Array and (ids_value as PackedInt32Array).size() > 0:
		biome_id = clampi(int((ids_value as PackedInt32Array)[0]), 0, BIOME_COUNT - 1)
	var strength: float = float(slot.get(&"strength"))
	# Walk the graph as an ordered chain from OUTPUT_DISPLACEMENT back through its
	# single input, so layers apply in the authored order and a stray disconnected
	# node is ignored.
	var by_id: Dictionary = {}
	var output_id: String = ""
	var blend_km: float = 0.0
	for value: Variant in nodes_value as Array:
		if not (value is Dictionary):
			continue
		var node: Dictionary = value as Dictionary
		by_id[String(node.get("id", ""))] = node
		if String(node.get("type", "")) == "OUTPUT_DISPLACEMENT":
			output_id = String(node.get("id", ""))
			blend_km = maxf(0.0, float((node.get("parameters", {}) as Dictionary).get(
				BIOME_BLEND_PARAM, 0.0)))
	var input_of: Dictionary = {}
	for link_value: Variant in links_value as Array:
		if not (link_value is Dictionary):
			continue
		var link: Dictionary = link_value as Dictionary
		if int(link.get("to_port", 0)) == 0:
			input_of[String(link.get("to", ""))] = String(link.get("from", ""))
	var ordered: Array = []
	var cursor: String = String(input_of.get(output_id, ""))
	var guard: int = 0
	while not cursor.is_empty() and by_id.has(cursor) and guard < 64:
		guard += 1
		ordered.push_front(by_id[cursor])
		cursor = String(input_of.get(cursor, ""))
	var layers: Array = []
	for node_value: Variant in ordered:
		var node: Dictionary = node_value as Dictionary
		var layer: Dictionary = _biome_layer_from_node(biome_id, strength,
			String(node.get("type", "")), node.get("parameters", {}) as Dictionary)
		if not layer.is_empty():
			layers.append(layer)
	return {"biome_id": biome_id, "blend_m": blend_km * 1000.0, "layers": layers}


## Mirrors _parse_biome_layer_slot's OUTPUT_DISPLACEMENT chain-walk, but every
## Biome Texture layer is the same node type ("TEXTURE_BAND") -- its
## parameters (height/slope range, colour, texture choice, noise), not a
## distinct node type, are what differentiate one layer from another, since
## there is no separate "shape" the way EROSION_CHANNELS vs RIDGED_MOUNTAINS
## is for height. Returns a flat Array of layer dicts (no wrapping
## {biome_id, layers} dict -- callers just extend _biome_textures with it).
func _parse_biome_texture_slot(slot: Resource) -> Array:
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return []
	var nodes_value: Variant = graph.get(&"nodes")
	var links_value: Variant = graph.get(&"links")
	if not (nodes_value is Array) or not (links_value is Array):
		return []
	var biome_id: int = 0
	var ids_value: Variant = slot.get(&"biome_ids")
	if ids_value is PackedInt32Array and (ids_value as PackedInt32Array).size() > 0:
		biome_id = clampi(int((ids_value as PackedInt32Array)[0]), 0, BIOME_COUNT - 1)
	var by_id: Dictionary = {}
	var output_id: String = ""
	for value: Variant in nodes_value as Array:
		if not (value is Dictionary):
			continue
		var node: Dictionary = value as Dictionary
		by_id[String(node.get("id", ""))] = node
		if String(node.get("type", "")) == "OUTPUT_DISPLACEMENT":
			output_id = String(node.get("id", ""))
	var input_of: Dictionary = {}
	for link_value: Variant in links_value as Array:
		if link_value is Dictionary and int((link_value as Dictionary).get("to_port", 0)) == 0:
			input_of[String((link_value as Dictionary).get("to", ""))] = \
				String((link_value as Dictionary).get("from", ""))
	var ordered: Array = []
	var cursor: String = String(input_of.get(output_id, ""))
	var guard: int = 0
	while not cursor.is_empty() and by_id.has(cursor) and guard < 64:
		guard += 1
		ordered.push_front(by_id[cursor])
		cursor = String(input_of.get(cursor, ""))
	var layers: Array = []
	for node_value: Variant in ordered:
		var node: Dictionary = node_value as Dictionary
		if String(node.get("type", "")) != "TEXTURE_BAND":
			continue
		var p: Dictionary = node.get("parameters", {}) as Dictionary
		var color := Color(0.5, 0.5, 0.5)
		var color_value: Variant = p.get("color", null)
		if color_value is Color:
			color = color_value
		elif color_value is Array and (color_value as Array).size() >= 3:
			var ca: Array = color_value
			color = Color(float(ca[0]), float(ca[1]), float(ca[2]))
		layers.append({
			"biome_id": biome_id,
			"texture_choice": clampi(int(p.get("texture_choice", 0)), 0, 4),
			"height_min": float(p.get("height_min", -1000000.0)),
			"height_max": float(p.get("height_max", 1000000.0)),
			"slope_min": clampf(float(p.get("slope_min", 0.0)), 0.0, 180.0),
			"slope_max": clampf(float(p.get("slope_max", 180.0)), 0.0, 180.0),
			"softness": maxf(float(p.get("softness", 5.0)), 0.0),
			"seed": int(p.get("seed", 1337)),
			"color": color,
			"opacity": clampf(float(p.get("opacity", 1.0)), 0.0, 1.0),
			"noise_scale": maxf(float(p.get("noise_scale", 0.0)), 0.0),
			"noise_strength": clampf(float(p.get("noise_strength", 0.0)), 0.0, 1.0),
			"tint_strength": clampf(float(p.get("tint_strength", 0.0)), 0.0, 1.0),
		})
	return layers


func _biome_layer_from_node(biome_id: int, strength: float, node_type: String,
		parameters: Dictionary) -> Dictionary:
	if not BIOME_LAYER_TYPE_BY_NODE.has(node_type):
		return {}
	var layer_type: int = int(BIOME_LAYER_TYPE_BY_NODE[node_type])
	var param: int = clampi(int(parameters.get("passes", 3)), 1, 4)
	if layer_type == 2:
		param = clampi(int(parameters.get("steps", 6)), 2, 24)
	elif layer_type == 3 or layer_type == 4:
		param = 3
	var amount: float = float(parameters.get("amount", 0.0)) * strength
	if is_zero_approx(amount):
		return {}
	return {
		"biome_id": biome_id,
		"layer_type": layer_type,
		"scale": maxf(0.0001, absf(float(parameters.get("scale", 6.0)))),
		"amount": amount,
		"param": param,
		"seed": int(parameters.get("seed", 1337 + biome_id * 101)),
		"angle_deg": float(parameters.get("angle_deg", 90.0)),
	}


# GDScript's float is double precision, so `direction * frequency` never
# aliases here even at extreme frequency -- no bounded/anchor-relative sample
# position, wrapping, or reference frame is needed on the CPU. The GPU side
# (shaders/terrain_biome_profile.gdshaderinc's bp_noise) samples the same
# `direction * frequency` coordinate, just recovers the precision a single
# float32 multiply would lose via an FMA-based float32-pair correction instead
# of double math -- so the two sides agree on phase again (unlike an earlier
# version of this fix, which sampled the GPU side off a moving reference frame
# and just relocated the popping to whenever that frame reset).
func _biome_layer_value(d: Vector3, layer: Dictionary) -> float:
	var layer_type: int = int(layer.get("layer_type", 0))
	var scale: float = float(layer.get("scale", 6.0))
	var amount: float = float(layer.get("amount", 0.0))
	var seed: int = int(layer.get("seed", 1337))
	var param: int = int(layer.get("param", 3))
	match layer_type:
		1:
			var ridge: float = 1.0 - absf(_terrain_fbm(d, scale, seed, param))
			return ridge * ridge * amount
		2:
			var field: float = _terrain_fbm(d, scale, seed, 1)
			var steps: float = float(clampi(param, 2, 24))
			var terrace: float = floor((field * 0.5 + 0.5) * steps) / maxf(steps - 1.0, 1.0)
			return (terrace * 2.0 - 1.0) * amount
		3:
			var angle_deg: float = float(layer.get("angle_deg", 90.0))
			var channel: float = _biome_erosion_channels(d, scale, seed, param, angle_deg)
			return -channel * amount
		4:
			var angle_deg2: float = float(layer.get("angle_deg", 90.0))
			var deposit: float = _biome_sediment_deposit(d, scale, seed, param, angle_deg2)
			return deposit * amount
		_:
			return _terrain_fbm(d, scale, seed, param) * amount


# Branchless orthonormal tangent basis (Duff et al. 2017); mirrors bp_tangent_basis.
func _biome_tangent_basis(n: Vector3) -> Array:
	var s: float = 1.0 if n.z >= 0.0 else -1.0
	var a: float = -1.0 / (s + n.z)
	var b: float = n.x * n.y * a
	return [
		Vector3(1.0 + s * n.x * n.x * a, s * b, -s * n.x),
		Vector3(b, s + n.y * n.y * a, -n.y),
	]


# Mirrors bp_rotate_basis: rotates an orthonormal tangent-plane basis by an
# authored angle, so a layer's flow/ridge orientation is a controllable
# parameter instead of whatever the raw (numerically-stable-but-arbitrary)
# tangent basis + the noise lattice's own slight anisotropy happen to give.
func _biome_rotate_basis(tx: Vector3, ty: Vector3, angle_deg: float) -> Array:
	var a: float = deg_to_rad(angle_deg)
	var ca: float = cos(a)
	var sa: float = sin(a)
	return [tx * ca + ty * sa, ty * ca - tx * sa]


# Mirrors bp_erosion_channels: domain-warped ridged noise so channels bend and
# branch like water-carved valleys instead of the isotropic blobs a plain
# ridged field gives. See the shader comment for why (a real flow-accumulation
# simulation needs the whole height field at once, which neither side has
# per-vertex/per-sample). Double precision means no bp_noise-style FMA
# correction is needed here -- direction * frequency is already exact.
# `angle_deg` rotates the flow/channel orientation (see _biome_rotate_basis).
func _biome_erosion_channels(d: Vector3, scale: float, seed: int, passes: int,
		angle_deg: float) -> float:
	var basis0: Array = _biome_tangent_basis(d)
	var rotated: Array = _biome_rotate_basis(basis0[0], basis0[1], angle_deg)
	var tx: Vector3 = rotated[0]
	var ty: Vector3 = rotated[1]
	# Mirrors the shader's base_frequency clamp for parity, not precision --
	# see BIOME_MAX_FREQUENCY.
	var base_frequency: float = minf(maxf(absf(scale), 0.000001), BIOME_MAX_FREQUENCY)
	var flow_frequency: float = base_frequency * 0.12
	var flow_x: float = _value_noise_3d(d * flow_frequency, seed + 5101)
	var flow_y: float = _value_noise_3d(d * flow_frequency, seed + 7247)
	var total: float = 0.0
	var normalizer: float = 0.0
	var amplitude: float = 1.0
	var frequency: float = base_frequency
	for octave: int in clampi(passes, 1, 4):
		var f: float = frequency
		var branch_x: float = _value_noise_3d(d * f, seed + octave * 733 + 11)
		var branch_y: float = _value_noise_3d(d * f, seed + octave * 733 + 19)
		# Mirrors the shader's float32-vanishing floor on warp_scale.
		var warp_scale: float = maxf(1.0 / maxf(f, 1.0), 3.0e-6)
		var warped: Vector3 = (d + (tx * (flow_x * 1.4 + branch_x * 0.5)
			+ ty * (flow_y * 1.4 + branch_y * 0.5)) * warp_scale).normalized()
		var ridge: float = 1.0 - absf(_value_noise_3d(warped * f, seed + octave * 1013))
		var channel: float = ridge * ridge * ridge
		total += channel * amplitude
		normalizer += amplitude
		frequency *= 2.0
		amplitude *= 0.5
	return clampf(total / normalizer, 0.0, 1.0) if normalizer > 0.0 else 0.0


# Mirrors bp_sediment_deposit: landslide-style talus, shifted toward a locally
# coherent "downhill bias" direction (two independent low-frequency noise
# fields, NOT a finite-difference gradient -- see the shader comment for why
# a gradient direction spikes here) and weighted by a steepness estimate so it
# tapers to nothing on flat ground. `angle_deg` rotates the bias direction
# (see _biome_rotate_basis).
func _biome_sediment_deposit(d: Vector3, scale: float, seed: int, passes: int,
		angle_deg: float) -> float:
	var basis0: Array = _biome_tangent_basis(d)
	var rotated: Array = _biome_rotate_basis(basis0[0], basis0[1], angle_deg)
	var tx: Vector3 = rotated[0]
	var ty: Vector3 = rotated[1]
	# Mirrors the shader's base_frequency clamp for parity, not precision --
	# see BIOME_MAX_FREQUENCY.
	var base_frequency: float = minf(maxf(absf(scale), 0.000001), BIOME_MAX_FREQUENCY)
	# Mirrors the shader: both the steepness estimate and the bias direction
	# work off a much lower frequency than the deposit texture itself (a
	# landslide responds to the broad slope, not noise-level bumps).
	var slope_frequency: float = maxf(base_frequency * 0.15, 0.000001)
	# Mirrors the shader's float32-vanishing floor on step_ang (see its
	# comment) so both sides estimate the same steepness even though the CPU's
	# double precision would not itself need the floor.
	var step_ang: float = maxf(0.6 / maxf(slope_frequency, 1.0), 3.0e-6)
	var h0: float = 1.0 - absf(_value_noise_3d(d * slope_frequency, seed + 9001))
	var dx: Vector3 = (d + tx * step_ang).normalized()
	var dy: Vector3 = (d + ty * step_ang).normalized()
	var hx: float = 1.0 - absf(_value_noise_3d(dx * slope_frequency, seed + 9001))
	var hy: float = 1.0 - absf(_value_noise_3d(dy * slope_frequency, seed + 9001))
	# Used only as a scalar blend weight below, never as a direction -- see the
	# shader comment on why that split matters.
	var steepness: float = Vector2(hx - h0, hy - h0).length()
	var slope_w: float = smoothstep(0.02, 0.35, steepness)

	var flow_x: float = _value_noise_3d(d * slope_frequency, seed + 5101)
	var flow_y: float = _value_noise_3d(d * slope_frequency, seed + 7247)
	var shift_scale: float = maxf(1.6 / maxf(slope_frequency, 1.0), 8.0e-6)
	# One shared downhill-bias shift for every octave -- see the shader comment.
	var shift: Vector2 = Vector2(flow_x, flow_y) * shift_scale

	var total: float = 0.0
	var normalizer: float = 0.0
	var amplitude: float = 1.0
	var frequency: float = base_frequency
	for octave: int in clampi(passes, 1, 4):
		var f: float = frequency
		var shifted: Vector3 = (d - (tx * shift.x + ty * shift.y)).normalized()
		var field: float = _value_noise_3d(shifted * f, seed + octave * 1013)
		var deposit: float = clampf(1.0 - absf(field * 1.65), 0.0, 1.0)
		total += deposit * deposit * amplitude
		normalizer += amplitude
		frequency *= 2.0
		amplitude *= 0.5
	var avg: float = clampf(total / normalizer, 0.0, 1.0) if normalizer > 0.0 else 0.0
	return avg * slope_w


# Best available biome id at a direction, matching what the render samples:
# the procedural context field when the planet is ready, else the authoring
# biome-preview image.
func _biome_id_at(direction: Vector3) -> int:
	if Planet.get("ready_state") and Planet.has_method("sample_info"):
		var info: Dictionary = Planet.call("sample_info", direction) as Dictionary
		return clampi(int(info.get("biome", 0)), 0, BIOME_COUNT - 1)
	return _sample_authored_biome(direction)


# 0..1 smooth membership of `biome` in a fixed tangent-plane Gaussian disc.
# Mirrors bp_biome_coverage. `centre_biome` is the caller's already-resolved biome
# under `d`; the offset taps are sampled here. Outside the band and at a 0
# half-width this agrees exactly with the render.
func _biome_coverage(d: Vector3, tx: Vector3, ty: Vector3, biome: int,
		centre_biome: int) -> float:
	var centre: float = 1.0 if centre_biome == biome else 0.0
	var half_m: float = _biome_blend_m[clampi(biome, 0, BIOME_COUNT - 1)]
	if half_m <= 1.0:
		return centre
	var radius: float = maxf(float(Planet.cfg.planet_radius), 1.0) if Planet.cfg != null else 1.0
	var step_ang: float = (half_m * 0.5) / radius
	# Spatially smooth kernel shift, mirrors bp_biome_coverage: dissolves the
	# fixed-grid quantisation into a ramp without per-vertex spikes. Frequency is
	# kept small so the noise stays smooth (see the shader comment).
	var shift_freq: float = clampf(radius / maxf(half_m * 3.0, 1.0), 8.0, 256.0)
	var jitter_p: Vector3 = d * shift_freq
	var shift := Vector2(
		_value_noise_3d(jitter_p, 40009), _value_noise_3d(jitter_p, 80021)) * 0.85
	var acc: float = 0.0
	var wsum: float = 0.0
	for j: int in range(-2, 3):
		for i: int in range(-2, 3):
			var ox: float = float(i) + shift.x
			var oy: float = float(j) + shift.y
			var w: float = exp(-(ox * ox + oy * oy) * 0.45)
			var sd: Vector3 = (d + (tx * ox + ty * oy) * step_ang).normalized()
			var hit: float = 1.0 if _biome_id_at(sd) == biome else 0.0
			acc += w * hit
			wsum += w
	return smoothstep(0.20, 0.80, acc / maxf(wsum, 1e-6))


# Metres the biome layer stack adds at `direction`, resolving the boundary blend
# itself. Mirrors shaders/terrain_biome_profile.gdshaderinc so contact/AGL tracks
# the render.
func _biome_profiles_height(direction: Vector3, biome_id: int) -> float:
	if _biome_profiles.is_empty():
		return 0.0
	var d: Vector3 = direction.normalized()
	var centre_biome: int = biome_id
	if centre_biome < 0:
		centre_biome = _biome_id_at(d)
	centre_biome = clampi(centre_biome, 0, BIOME_COUNT - 1)
	var basis: Array = _biome_tangent_basis(d)
	var tx: Vector3 = basis[0]
	var ty: Vector3 = basis[1]
	var total: float = 0.0
	var cached_biome: int = -1
	var cached_coverage: float = 0.0
	for entry_value: Variant in _biome_profiles:
		var entry: Dictionary = entry_value as Dictionary
		var layer_biome: int = int(entry.get("biome_id", -1))
		if layer_biome != cached_biome:
			cached_biome = layer_biome
			cached_coverage = _biome_coverage(d, tx, ty, layer_biome, centre_biome)
		if cached_coverage <= 0.0:
			continue
		total += _biome_layer_value(d, entry) * cached_coverage
	return total


# {count, a, b, blend} packed for shaders/terrain_biome_profile.gdshaderinc.
func biome_profile_uniforms() -> Dictionary:
	var count: int = mini(_biome_profiles.size(), BIOME_LAYER_MAX)
	var a := PackedVector4Array()
	var b := PackedVector4Array()
	a.resize(BIOME_LAYER_MAX)
	b.resize(BIOME_LAYER_MAX)
	for i: int in count:
		var entry: Dictionary = _biome_profiles[i] as Dictionary
		a[i] = Vector4(float(int(entry.get("biome_id", 0))), float(int(entry.get("layer_type", 0))),
			float(entry.get("scale", 6.0)), float(entry.get("amount", 0.0)))
		b[i] = Vector4(float(int(entry.get("param", 3))), float(int(entry.get("seed", 1337))),
			float(entry.get("angle_deg", 90.0)), 0.0)
	return {"count": count, "a": a, "b": b, "blend": _biome_blend_m.duplicate()}


func biome_profile_count() -> int:
	return mini(_biome_profiles.size(), BIOME_LAYER_MAX)


# {count, a, b, c, d} packed for shaders/terrain_biome_texture.gdshaderinc.
func biome_texture_uniforms() -> Dictionary:
	var count: int = mini(_biome_textures.size(), BIOME_TEX_LAYER_MAX)
	var a := PackedVector4Array()
	var b := PackedVector4Array()
	var c := PackedVector4Array()
	var d := PackedVector4Array()
	a.resize(BIOME_TEX_LAYER_MAX)
	b.resize(BIOME_TEX_LAYER_MAX)
	c.resize(BIOME_TEX_LAYER_MAX)
	d.resize(BIOME_TEX_LAYER_MAX)
	for i: int in count:
		var entry: Dictionary = _biome_textures[i] as Dictionary
		a[i] = Vector4(float(int(entry.get("biome_id", 0))), float(int(entry.get("texture_choice", 0))),
			float(entry.get("height_min", -1000000.0)), float(entry.get("height_max", 1000000.0)))
		b[i] = Vector4(float(entry.get("slope_min", 0.0)), float(entry.get("slope_max", 180.0)),
			float(entry.get("softness", 5.0)), float(int(entry.get("seed", 1337))))
		var col: Color = entry.get("color", Color(0.5, 0.5, 0.5)) as Color
		c[i] = Vector4(col.r, col.g, col.b, float(entry.get("opacity", 1.0)))
		d[i] = Vector4(float(entry.get("noise_scale", 0.0)), float(entry.get("noise_strength", 0.0)),
			float(entry.get("tint_strength", 0.0)), 0.0)
	return {"count": count, "a": a, "b": b, "c": c, "d": d}


func biome_texture_count() -> int:
	return mini(_biome_textures.size(), BIOME_TEX_LAYER_MAX)


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
