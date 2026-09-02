extends "res://scripts/world_authoring/terrain_displacement_runtime_phase37.gd"
## Phase 41: deterministic planet-space latitude/longitude masks for authored displacement.
##
## Spatial masks are deliberately implemented in the same bytecode VM used by the
## rendered terrain and the CPU/contact reference evaluator. They are NOT accepted
## by Phase 40's resident native-stage coefficient lowering yet; that path remains
## constant-only until native stage provenance can carry spatial factors exactly.
##
## Opcode 27 remains backward compatible with the original Latitude Mask contract.
## The fourth parameter encodes axis + invert:
##   0 latitude, 1 latitude inverted, 2 longitude, 3 longitude inverted.

const OP_LATITUDE_MASK: int = 27
const LATITUDE_MASK_TYPE := "LATITUDE_MASK"
const MASK_AXIS_LATITUDE := "latitude"
const MASK_AXIS_LONGITUDE := "longitude"


func _compile_node(node_id: String, nodes: Dictionary, inputs: Dictionary,
		memo: Dictionary, visiting: Dictionary, graph_seed: int) -> int:
	if memo.has(node_id):
		return int(memo[node_id])
	if nodes.has(node_id):
		var node: Dictionary = nodes[node_id] as Dictionary
		if String(node.get("type", "")) == LATITUDE_MASK_TYPE:
			var parameters: Dictionary = node.get("parameters", {}) as Dictionary
			var axis: String = String(parameters.get("axis", MASK_AXIS_LATITUDE)).to_lower()
			var edge_a_deg: float = float(parameters.get("south_deg", -30.0))
			var edge_b_deg: float = float(parameters.get("north_deg", 30.0))
			var feather_deg: float = float(parameters.get("feather_deg", 5.0))
			var invert_mask: bool = bool(parameters.get("invert", false))
			if not is_finite(edge_a_deg) or not is_finite(edge_b_deg) \
					or not is_finite(feather_deg):
				_warnings.append("Spatial Band contains a non-finite setting; candidate rejected.")
				return -1
			var axis_code: float = 0.0
			match axis:
				MASK_AXIS_LATITUDE:
					if edge_a_deg < -90.0 or edge_a_deg > 90.0 \
							or edge_b_deg < -90.0 or edge_b_deg > 90.0:
						_warnings.append("Latitude Mask limits must stay between -90 and +90 degrees; candidate rejected.")
						return -1
					if feather_deg < 0.0 or feather_deg > 90.0:
						_warnings.append("Latitude Mask feather must stay between 0 and 90 degrees; candidate rejected.")
						return -1
				MASK_AXIS_LONGITUDE:
					if edge_a_deg < -180.0 or edge_a_deg > 180.0 \
							or edge_b_deg < -180.0 or edge_b_deg > 180.0:
						_warnings.append("Longitude Mask limits must stay between -180 and +180 degrees; candidate rejected.")
						return -1
					if feather_deg < 0.0 or feather_deg > 180.0:
						_warnings.append("Longitude Mask feather must stay between 0 and 180 degrees; candidate rejected.")
						return -1
					axis_code = 2.0
				_:
					_warnings.append("Spatial Band axis must be latitude or longitude; candidate rejected.")
					return -1
			var flags: float = axis_code + (1.0 if invert_mask else 0.0)
			var instruction: int = _append_instruction(OP_LATITUDE_MASK, -1, -1, -1,
				Vector4(edge_a_deg, edge_b_deg, feather_deg, flags))
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
			OP_LATITUDE_MASK:
				out = _longitude_mask_value(d, p) if int(round(p.w)) >= 2 \
					else _latitude_mask_value(d, p)
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


func _compiled_program_bounds() -> Dictionary:
	if not _active or _output_index < 0 or _headers.is_empty():
		return {"known": true, "min_m": 0.0, "max_m": 0.0, "reason": ""}

	var registers: Array[Dictionary] = []
	for index: int in _headers.size():
		var h: Vector4 = _headers[index]
		var p: Vector4 = _params[index]
		var op: int = int(round(h.x))
		var a: Dictionary = _bound_register(registers, int(round(h.y)), index)
		var b: Dictionary = _bound_register(registers, int(round(h.z)), index)
		var c: Dictionary = _bound_register(registers, int(round(h.w)), index)
		var out: Dictionary
		match op:
			OP_CONST:
				out = _bound(float(p.x), float(p.x))
			OP_INPUT_BASE_HEIGHT:
				out = _bound(0.0, 0.0, 1.0, 0.0)
			OP_INPUT_SCULPT_DELTA:
				out = _bound(0.0, 0.0, 0.0, 1.0)
			OP_INPUT_BIOME:
				out = _bound(0.0, float(BIOME_COUNT - 1))
			OP_INPUT_LEVEL:
				out = _bound(0.0, 30.0)
			OP_INPUT_TIME:
				out = _unknown_bound("time-dependent displacement has no static bound yet")
			OP_NOISE:
				out = _bound(-1.0, 1.0)
			OP_LATITUDE_MASK:
				# Spatial localization is deliberately NOT used to shrink culling bounds.
				# Only the scalar range is propagated, preserving one global conservative
				# displacement envelope across every cube face and planet-space band.
				out = _bound(0.0, 1.0)
			OP_ADD:
				out = _add_bounds(a, b, 1.0)
			OP_SUB:
				out = _add_bounds(a, b, -1.0)
			OP_MUL:
				out = _multiply_bounds(a, b)
			OP_DIV:
				out = _divide_bounds(a, b)
			OP_MIN:
				out = _minmax_bounds(a, b, false)
			OP_MAX:
				out = _minmax_bounds(a, b, true)
			OP_ABS:
				out = _abs_bound(a)
			OP_POWER:
				out = _power_bound(a, b)
			OP_CLAMP:
				out = _clamp_bound(a, b, c)
			OP_SMOOTHSTEP:
				out = _bound(0.0, 1.0) if _all_pure([a, b, c]) \
					else _unknown_bound("Smoothstep depends on absolute production/sculpt height")
			OP_REMAP:
				out = _remap_bound(a, b, c, float(p.x), float(p.y))
			OP_MIX:
				out = _mix_bound(a, b, c)
			OP_SCALE:
				out = _scale_bound(a, float(p.x))
			OP_LEVEL_MASK, OP_BIOME_MASK:
				out = _masked_bound(a)
			OP_REPLACE:
				out = b.duplicate(true)
			OP_NOISE_LAYER:
				out = _add_interval_to_affine(a, -absf(float(p.y)), absf(float(p.y)))
			OP_RIDGED_MOUNTAINS:
				out = _add_interval_to_affine(a, minf(0.0, float(p.y)), maxf(0.0, float(p.y)))
			OP_EROSION_CHANNELS:
				out = _add_interval_to_affine(a, minf(-float(p.y), 0.0), maxf(-float(p.y), 0.0))
			OP_SEDIMENT_DEPOSIT:
				out = _add_interval_to_affine(a, minf(0.0, float(p.y)), maxf(0.0, float(p.y)))
			_:
				out = _unknown_bound("opcode %d has no conservative bound rule" % op)
		registers.append(out)

	if _output_index < 0 or _output_index >= registers.size():
		return {"known": false, "min_m": 0.0, "max_m": 0.0,
			"reason": "compiled output register is invalid"}
	var result: Dictionary = registers[_output_index]
	if not bool(result.get("known", false)):
		return {"known": false, "min_m": 0.0, "max_m": 0.0,
			"reason": String(result.get("reason", "compiled displacement bound is unknown"))}
	if absf(float(result.get("base", 0.0))) > COEFFICIENT_EPSILON \
			or absf(float(result.get("sculpt", 0.0))) > COEFFICIENT_EPSILON:
		return {"known": false, "min_m": 0.0, "max_m": 0.0,
			"reason": "graph changes absolute production/sculpt height in a way that is not statically bounded"}
	return {
		"known": true,
		"min_m": float(result.get("min", 0.0)),
		"max_m": float(result.get("max", 0.0)),
		"reason": "",
	}


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
		lower = _smoothstep_static(south_deg - feather_deg, south_deg, latitude_deg)
		upper = 1.0 - _smoothstep_static(north_deg, north_deg + feather_deg, latitude_deg)
	var value: float = clampf(lower * upper, 0.0, 1.0)
	return 1.0 - value if (int(round(parameters.w)) & 1) != 0 else value


static func longitude_mask_value(direction: Vector3, west_deg: float,
		east_deg: float, feather_deg: float = 5.0, invert_mask: bool = false) -> float:
	if direction.length_squared() <= 1e-12:
		return 0.0
	return _longitude_mask_value(direction.normalized(),
		Vector4(west_deg, east_deg, feather_deg, 2.0 + (1.0 if invert_mask else 0.0)))


static func _longitude_mask_value(direction_normalized: Vector3, parameters: Vector4) -> float:
	# Coordinate convention matches the GPU VM: 0° = +Z and +90° = +X.
	var longitude_deg: float = rad_to_deg(atan2(direction_normalized.x, direction_normalized.z))
	var west_deg: float = _wrap_degrees(parameters.x)
	var east_deg: float = _wrap_degrees(parameters.y)
	var feather_deg: float = maxf(parameters.z, 0.0)
	var span_deg: float = fposmod(east_deg - west_deg, 360.0)
	var along_deg: float = fposmod(longitude_deg - west_deg, 360.0)
	var value: float = 0.0
	if span_deg <= 1e-6:
		# Equal edges intentionally mean the entire circumference.
		value = 1.0
	elif along_deg <= span_deg:
		value = 1.0
		if feather_deg > 1e-6:
			var from_west: float = along_deg
			var from_east: float = span_deg - along_deg
			value = minf(
				_smoothstep_static(0.0, feather_deg, from_west),
				_smoothstep_static(0.0, feather_deg, from_east))
	elif feather_deg > 1e-6:
		var after_east: float = along_deg - span_deg
		var before_west: float = 360.0 - along_deg
		var edge_distance: float = minf(after_east, before_west)
		value = 1.0 - _smoothstep_static(0.0, feather_deg, edge_distance)
	value = clampf(value, 0.0, 1.0)
	return 1.0 - value if (int(round(parameters.w)) & 1) != 0 else value


static func _wrap_degrees(degrees: float) -> float:
	return fposmod(degrees + 180.0, 360.0) - 180.0


static func _smoothstep_static(edge0: float, edge1: float, value: float) -> float:
	if absf(edge1 - edge0) <= 1e-12:
		return 0.0 if value < edge0 else 1.0
	var t: float = clampf((value - edge0) / (edge1 - edge0), 0.0, 1.0)
	return t * t * (3.0 - 2.0 * t)


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["latitude_mask_opcode"] = OP_LATITUDE_MASK
	out["spatial_latitude_mask"] = true
	out["spatial_longitude_mask"] = true
	return out
