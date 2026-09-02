extends "res://scripts/world_authoring/terrain_displacement_runtime_phase29.gd"
## Phase 30 authoritative shape inputs.
##
## Phase 29 made the production result editable, but it still appeared as one
## CURRENT TERRAIN HEIGHT black box. Phase 30 exposes the two stages that actually
## enter the local rendered/contact surface at this point: generated terrain and
## sparse sculpt/edit delta. The absolute graph compiles relative to their combined
## baseline so the inherited renderer can keep its mature post-morph edit path
## without applying sculpt twice.

const OP_INPUT_SCULPT_DELTA: int = 26
const PRODUCTION_SHAPE_SLOT_ID := "production-terrain-shape"


func compile_from_terrain(terrain: Resource) -> Dictionary:
	# The visible default graph is an identity view of generated+sculpt terrain and
	# therefore does not need bytecode. Previous revisions temporarily disabled that
	# slot on the LIVE authoring Resource while compiling. Interactive editing makes
	# that unsafe: observers can see the temporary state and stale callbacks can race
	# it. Build a detached profile instead; the source document is never mutated.
	if terrain == null:
		return super.compile_from_terrain(terrain)
	var source_fingerprint: String = profile_fingerprint(terrain)
	var compile_profile: Resource = _detached_compile_profile(terrain)
	if compile_profile == null:
		return super.compile_from_terrain(terrain)

	var slots_value: Variant = compile_profile.get(&"displacement_slots")
	if slots_value is Array:
		for slot_value: Variant in slots_value as Array:
			var slot: Resource = slot_value as Resource
			if slot != null and bool(slot.get(&"enabled")) and _is_identity_shape_slot(slot):
				slot.set(&"enabled", false)

	var result: Dictionary = super.compile_from_terrain(compile_profile)
	# The candidate was compiled from the detached view, but polling must compare
	# against the real authoring document. Retagging changes metadata only; it never
	# changes bytecode or a material binding.
	retag_current_profile(source_fingerprint)
	return stats()


func _detached_compile_profile(terrain: Resource) -> Resource:
	var detached: Resource = terrain.duplicate(false)
	if detached == null:
		return null
	var slots_value: Variant = terrain.get(&"displacement_slots")
	if not (slots_value is Array):
		return detached
	var detached_slots: Array[Resource] = []
	for slot_value: Variant in slots_value as Array:
		var source_slot: Resource = slot_value as Resource
		if source_slot == null:
			detached_slots.append(null)
			continue
		var detached_slot: Resource = source_slot.duplicate(false)
		if detached_slot == null:
			return null
		var source_graph: Resource = source_slot.get(&"graph") as Resource
		if source_graph != null:
			var detached_graph: Resource = source_graph.duplicate(true)
			if detached_graph == null:
				return null
			detached_slot.set(&"graph", detached_graph)
		detached_slots.append(detached_slot)
	detached.set(&"displacement_slots", detached_slots)
	return detached


func _is_identity_shape_slot(slot: Resource) -> bool:
	if String(slot.get(&"slot_id")) != PRODUCTION_SHAPE_SLOT_ID:
		return false
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null or int(graph.get(&"displacement_output_mode")) != GRAPH_OUTPUT_ABSOLUTE_HEIGHT:
		return false
	var nodes: Array = graph.get(&"nodes") as Array
	var links: Array = graph.get(&"links") as Array
	if nodes.size() != 4 or links.size() != 3:
		return false
	var generated_id: String = ""
	var sculpt_id: String = ""
	var add_id: String = ""
	var output_id: String = ""
	for node_value: Variant in nodes:
		var node: Dictionary = node_value as Dictionary
		var node_type: String = String(node.get("type", ""))
		var node_id: String = String(node.get("id", ""))
		if node_type == "GAME_INPUT":
			var parameters: Dictionary = node.get("parameters", {}) as Dictionary
			var source: String = String(parameters.get("source", ""))
			if source == "generated_height_m":
				generated_id = node_id
			elif source == "sculpt_delta_m":
				sculpt_id = node_id
			else:
				return false
		elif node_type == "ADD":
			add_id = node_id
		elif node_type == "OUTPUT_DISPLACEMENT":
			output_id = node_id
		else:
			return false
	if generated_id.is_empty() or sculpt_id.is_empty() or add_id.is_empty() or output_id.is_empty():
		return false
	var expected: Dictionary = {
		"%s:0" % add_id: generated_id,
		"%s:1" % add_id: sculpt_id,
		"%s:0" % output_id: add_id,
	}
	for link_value: Variant in links:
		var link: Dictionary = link_value as Dictionary
		var key: String = "%s:%d" % [String(link.get("to", "")), int(link.get("to_port", -1))]
		if String(expected.get(key, "")) != String(link.get("from", "")):
			return false
		expected.erase(key)
	return expected.is_empty()


func _compile_graph(graph: Resource, graph_seed_index: int) -> int:
	var value_index: int = super._compile_graph(graph, graph_seed_index)
	if value_index < 0:
		return value_index
	if int(graph.get(&"displacement_output_mode")) == GRAPH_OUTPUT_ABSOLUTE_HEIGHT:
		# Phase 29 already converted absolute -> delta by subtracting generated base.
		# Subtract sculpt as well. The clipmap/contact caller adds its normal sculpt
		# path afterward, making the unchanged graph exactly identity while allowing
		# the graph to scale/remove/remap sculpt explicitly.
		var sculpt_index: int = _append_instruction(OP_INPUT_SCULPT_DELTA)
		if sculpt_index < 0:
			return -1
		value_index = _append_instruction(OP_SUB, value_index, sculpt_index)
	return value_index


func _compile_node(node_id: String, nodes: Dictionary, inputs: Dictionary,
		memo: Dictionary, visiting: Dictionary, graph_seed: int) -> int:
	if memo.has(node_id):
		return int(memo[node_id])
	if nodes.has(node_id):
		var node: Dictionary = nodes[node_id] as Dictionary
		if String(node.get("type", "")) == "GAME_INPUT":
			var parameters: Dictionary = node.get("parameters", {}) as Dictionary
			var source: String = String(parameters.get("source", ""))
			if source == "sculpt_delta_m":
				var sculpt: int = _append_instruction(OP_INPUT_SCULPT_DELTA)
				memo[node_id] = sculpt
				return sculpt
			if source == "generated_height_m":
				var generated: int = _append_instruction(OP_INPUT_BASE_HEIGHT)
				memo[node_id] = generated
				return generated
	return super._compile_node(node_id, nodes, inputs, memo, visiting, graph_seed)


func evaluate_height(direction: Vector3, base_height_m: float = 0.0,
		clipmap_level: int = 0, biome_id: int = -1, time_s: float = NAN,
		sculpt_delta_m: float = 0.0) -> float:
	if not _active or _output_index < 0 or direction.length_squared() <= 1e-12:
		return 0.0
	var d: Vector3 = direction.normalized()
	var resolved_biome: int = biome_id
	if resolved_biome < 0:
		resolved_biome = _sample_authored_biome(d)
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
			OP_INPUT_SCULPT_DELTA: out = sculpt_delta_m
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
			_: out = 0.0
		values[index] = out if is_finite(out) else 0.0
	return float(values[_output_index]) if _output_index < values.size() else 0.0
