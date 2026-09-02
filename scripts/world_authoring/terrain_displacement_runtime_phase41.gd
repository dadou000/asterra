extends "res://scripts/world_authoring/terrain_displacement_runtime_phase37.gd"
## Phase 41: deterministic planet-space latitude masks for authored displacement.
##
## Latitude Mask is deliberately implemented in the same bytecode VM used by the
## rendered terrain and the CPU/contact reference evaluator. It is NOT accepted by
## Phase 40's resident native-stage coefficient lowering yet; that path remains
## constant-only until native stage provenance can carry spatial factors exactly.

const OP_LATITUDE_MASK: int = 27
const LATITUDE_MASK_TYPE := "LATITUDE_MASK"


func _compile_node(node_id: String, nodes: Dictionary, inputs: Dictionary,
		memo: Dictionary, visiting: Dictionary, graph_seed: int) -> int:
	if memo.has(node_id):
		return int(memo[node_id])
	if nodes.has(node_id):
		var node: Dictionary = nodes[node_id] as Dictionary
		if String(node.get("type", "")) == LATITUDE_MASK_TYPE:
			var parameters: Dictionary = node.get("parameters", {}) as Dictionary
			var south_deg: float = float(parameters.get("south_deg", -30.0))
			var north_deg: float = float(parameters.get("north_deg", 30.0))
			var feather_deg: float = float(parameters.get("feather_deg", 5.0))
			var invert_mask: bool = bool(parameters.get("invert", false))
			if not is_finite(south_deg) or not is_finite(north_deg) \
					or not is_finite(feather_deg):
				_warnings.append("Latitude Mask contains a non-finite setting; candidate rejected.")
				return -1
			if south_deg < -90.0 or south_deg > 90.0 \
					or north_deg < -90.0 or north_deg > 90.0:
				_warnings.append("Latitude Mask limits must stay between -90 and +90 degrees; candidate rejected.")
				return -1
			if feather_deg < 0.0 or feather_deg > 90.0:
				_warnings.append("Latitude Mask feather must stay between 0 and 90 degrees; candidate rejected.")
				return -1
			var instruction: int = _append_instruction(OP_LATITUDE_MASK, -1, -1, -1,
				Vector4(south_deg, north_deg, feather_deg, 1.0 if invert_mask else 0.0))
			memo[node_id] = instruction
			return instruction
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
			OP_LATITUDE_MASK: out = _latitude_mask_value(d, p)
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


static func latitude_mask_value(direction: Vector3, south_deg: float,
		north_deg: float, feather_deg: float = 5.0, invert_mask: bool = false) -> float:
	if direction.length_squared() <= 1e-12:
		return 0.0
	return _latitude_mask_value(direction.normalized(),
		Vector4(south_deg, north_deg, feather_deg, 1.0 if invert_mask else 0.0))


static func _latitude_mask_value(direction_normalized: Vector3, parameters: Vector4) -> float:
	var latitude_deg: float = rad_to_deg(asin(clampf(direction_normalized.y, -1.0, 1.0)))
	var south_deg: float = minf(parameters.x, parameters.y)
	var north_deg: float = maxf(parameters.x, parameters.y)
	var feather_deg: float = maxf(parameters.z, 0.0)
	var lower: float = 1.0 if latitude_deg >= south_deg else 0.0
	var upper: float = 1.0 if latitude_deg <= north_deg else 0.0
	if feather_deg > 1e-6:
		lower = smoothstep(south_deg - feather_deg, south_deg, latitude_deg)
		upper = 1.0 - smoothstep(north_deg, north_deg + feather_deg, latitude_deg)
	var value: float = clampf(lower * upper, 0.0, 1.0)
	return 1.0 - value if parameters.w >= 0.5 else value


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["latitude_mask_opcode"] = OP_LATITUDE_MASK
	out["spatial_latitude_mask"] = true
	return out
