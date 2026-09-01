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
