extends "res://scripts/world_authoring/terrain_displacement_runtime_phase32.gd"
## Phase 33: transactional production controls + conservative displacement bounds.
##
## A graph candidate is not allowed to leak its production geomorph settings when
## the transactional bytecode compiler rejects that candidate. The active controls
## and active bytecode therefore advance together.
##
## This layer also derives a conservative radial displacement envelope from the
## exact production geomorph equations and, when possible, from the compiled author
## bytecode. The renderer consumes this envelope for LOD/horizon safety; unknown
## advanced programs deliberately fail open to a large renderer-side guard rather
## than risking disappearing terrain.

const FBM_ABS_BOUND: float = 1.0
const COEFFICIENT_EPSILON: float = 0.000001

var _last_displacement_envelope: Dictionary = {}


func clear() -> void:
	super.clear()
	_last_displacement_envelope = _calculate_displacement_envelope()


func compile_from_terrain(terrain: Resource) -> Dictionary:
	# Phase 32 extracts controls before entering the transactional compiler. Keep a
	# copy so rejected graph topology cannot still alter the production shader.
	var previous_controls: Dictionary = _production_controls.duplicate(true)
	var result: Dictionary = super.compile_from_terrain(terrain)
	if result.has("candidate_valid") and not bool(result.get("candidate_valid", true)):
		_production_controls = previous_controls

	_last_displacement_envelope = _calculate_displacement_envelope()
	# Always report the controls/envelope that are actually active after the
	# transaction, not the rejected candidate values returned by Phase 32.
	result["production_geomorph_controls"] = _production_controls.duplicate(true)
	result["displacement_envelope"] = _last_displacement_envelope.duplicate(true)
	return result


func displacement_envelope() -> Dictionary:
	if _last_displacement_envelope.is_empty():
		_last_displacement_envelope = _calculate_displacement_envelope()
	return _last_displacement_envelope.duplicate(true)


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["displacement_envelope"] = displacement_envelope()
	return out


func _calculate_displacement_envelope() -> Dictionary:
	var production_guard: float = _production_geomorph_guard_m()
	var program: Dictionary = _compiled_program_bounds()
	var program_known: bool = bool(program.get("known", false))
	var program_min: float = float(program.get("min_m", 0.0)) if program_known else 0.0
	var program_max: float = float(program.get("max_m", 0.0)) if program_known else 0.0
	var total_min: float = -production_guard + program_min
	var total_max: float = production_guard + program_max
	return {
		"bounds_known": program_known,
		"production_max_abs_m": production_guard,
		"author_program_min_m": program_min,
		"author_program_max_m": program_max,
		"author_program_max_abs_m": maxf(absf(program_min), absf(program_max)) if program_known else 0.0,
		"total_min_m": total_min if program_known else -production_guard,
		"total_max_m": total_max if program_known else production_guard,
		"total_max_abs_m": maxf(absf(total_min), absf(total_max)) if program_known else production_guard,
		"unknown_reason": String(program.get("reason", "")),
	}


func _production_geomorph_guard_m() -> float:
	# Every stochastic primitive used by gpu_geomorph.gdshaderinc is bounded to
	# [-1,1] (or [0,1] for ridge/cellular terms). Landform/context weights are also
	# clamped to [0,1]. Summing each band's worst possible contribution therefore
	# gives a strict, intentionally conservative radial envelope.
	var c: Dictionary = _production_controls
	var broad_amp: float = maxf(
		maxf(float(c.get("broad_low_amplitude_m", 24.0)), 0.0),
		maxf(float(c.get("broad_mountain_amplitude_m", 125.0)), 0.0))
	var broad: float = broad_amp * maxf(float(c.get("broad_strength", 1.0)), 0.0) \
		* FBM_ABS_BOUND

	# hardness = 1.20 - geology.r * 0.48, with geology.r in [0,1].
	var mountain: float = maxf(float(c.get("mountain_amplitude_m", 210.0)), 0.0) \
		* maxf(float(c.get("mountain_strength", 1.0)), 0.0) * 1.20

	var mid: float = (
		maxf(float(c.get("mid_ridge_amplitude_m", 72.0)), 0.0)
		+ maxf(float(c.get("mid_noise_amplitude_m", 24.0)), 0.0) * FBM_ABS_BOUND
	) * maxf(float(c.get("mid_strength", 1.0)), 0.0)

	var channels: float = maxf(float(c.get("channel_depth_max_m", 34.0)), 0.0) \
		* maxf(float(c.get("channel_strength", 1.0)), 0.0)
	var deposit: float = maxf(float(c.get("deposit_amplitude_max_m", 12.0)), 0.0) \
		* maxf(float(c.get("deposit_strength", 1.0)), 0.0)
	var fine_strength: float = maxf(float(c.get("fine_strength", 1.0)), 0.0)
	var fine: float = maxf(float(c.get("fine_amplitude_m", 4.5)), 0.0) \
		* fine_strength * FBM_ABS_BOUND
	var dune: float = maxf(float(c.get("dune_amplitude_m", 9.0)), 0.0) \
		* maxf(float(c.get("dune_strength", 1.0)), 0.0)
	var micro: float = maxf(float(c.get("micro_amplitude_m", 0.9)), 0.0) * fine_strength

	var pre_glacial: float = broad + mountain + mid + channels + deposit + fine + dune + micro
	var glacial_strength: float = maxf(float(c.get("glacial_strength", 1.0)), 0.0)
	var glacial_base_scale: float = maxf(float(c.get("glacial_base_scale", 0.62)), 0.0)
	var glacial_extra: float = maxf(float(c.get("glacial_amplitude_m", 52.0)), 0.0) \
		* glacial_strength * FBM_ABS_BOUND
	# The shader mixes h with h*base_scale + ice*amplitude. A convex mix cannot
	# exceed the larger endpoint envelope.
	var after_glacial: float = maxf(
		pre_glacial,
		pre_glacial * glacial_base_scale + glacial_extra)
	return after_glacial * maxf(float(c.get("detail_strength", 1.0)), 0.0)


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


func _bound(minimum: float, maximum: float, base: float = 0.0,
		sculpt: float = 0.0) -> Dictionary:
	return {"known": true, "min": minf(minimum, maximum), "max": maxf(minimum, maximum),
		"base": base, "sculpt": sculpt, "reason": ""}


func _unknown_bound(reason: String) -> Dictionary:
	return {"known": false, "min": 0.0, "max": 0.0, "base": 0.0,
		"sculpt": 0.0, "reason": reason}


func _bound_register(registers: Array[Dictionary], register_index: int,
		current: int) -> Dictionary:
	if register_index < 0:
		return _bound(0.0, 0.0)
	if register_index >= current or register_index >= registers.size():
		return _unknown_bound("compiled program references an invalid register")
	return registers[register_index]


func _is_pure(value: Dictionary) -> bool:
	return bool(value.get("known", false)) \
		and absf(float(value.get("base", 0.0))) <= COEFFICIENT_EPSILON \
		and absf(float(value.get("sculpt", 0.0))) <= COEFFICIENT_EPSILON


func _all_pure(values: Array) -> bool:
	for value: Variant in values:
		if not (value is Dictionary) or not _is_pure(value as Dictionary):
			return false
	return true


func _is_exact_scalar(value: Dictionary) -> bool:
	return _is_pure(value) and is_equal_approx(
		float(value.get("min", 0.0)), float(value.get("max", 0.0)))


func _add_bounds(a: Dictionary, b: Dictionary, b_sign: float) -> Dictionary:
	if not bool(a.get("known", false)):
		return a.duplicate(true)
	if not bool(b.get("known", false)):
		return b.duplicate(true)
	var b_min: float = float(b.get("min", 0.0)) * b_sign
	var b_max: float = float(b.get("max", 0.0)) * b_sign
	return _bound(
		float(a.get("min", 0.0)) + minf(b_min, b_max),
		float(a.get("max", 0.0)) + maxf(b_min, b_max),
		float(a.get("base", 0.0)) + float(b.get("base", 0.0)) * b_sign,
		float(a.get("sculpt", 0.0)) + float(b.get("sculpt", 0.0)) * b_sign)


func _scale_bound(value: Dictionary, scalar: float) -> Dictionary:
	if not bool(value.get("known", false)) or not is_finite(scalar):
		return _unknown_bound(String(value.get("reason", "invalid scale")))
	var lo: float = float(value.get("min", 0.0)) * scalar
	var hi: float = float(value.get("max", 0.0)) * scalar
	return _bound(minf(lo, hi), maxf(lo, hi),
		float(value.get("base", 0.0)) * scalar,
		float(value.get("sculpt", 0.0)) * scalar)


func _multiply_bounds(a: Dictionary, b: Dictionary) -> Dictionary:
	if _is_exact_scalar(a):
		return _scale_bound(b, float(a.get("min", 0.0)))
	if _is_exact_scalar(b):
		return _scale_bound(a, float(b.get("min", 0.0)))
	if not _all_pure([a, b]):
		return _unknown_bound("Multiply combines absolute production/sculpt height with a varying value")
	var products: Array[float] = [
		float(a.get("min", 0.0)) * float(b.get("min", 0.0)),
		float(a.get("min", 0.0)) * float(b.get("max", 0.0)),
		float(a.get("max", 0.0)) * float(b.get("min", 0.0)),
		float(a.get("max", 0.0)) * float(b.get("max", 0.0)),
	]
	return _bound(products.min(), products.max())


func _divide_bounds(a: Dictionary, b: Dictionary) -> Dictionary:
	if _is_exact_scalar(b):
		var divisor: float = float(b.get("min", 0.0))
		if absf(divisor) <= 0.000000000001:
			return _bound(0.0, 0.0)
		return _scale_bound(a, 1.0 / divisor)
	if not _all_pure([a, b]):
		return _unknown_bound("Divide uses an unbounded production/sculpt-height denominator")
	var b_min: float = float(b.get("min", 0.0))
	var b_max: float = float(b.get("max", 0.0))
	if b_min <= 0.000000000001 and b_max >= -0.000000000001:
		return _unknown_bound("Divide denominator can approach zero")
	var quotients: Array[float] = [
		float(a.get("min", 0.0)) / b_min,
		float(a.get("min", 0.0)) / b_max,
		float(a.get("max", 0.0)) / b_min,
		float(a.get("max", 0.0)) / b_max,
	]
	return _bound(quotients.min(), quotients.max())


func _minmax_bounds(a: Dictionary, b: Dictionary, want_max: bool) -> Dictionary:
	if not _all_pure([a, b]):
		return _unknown_bound("Min/Max depends on absolute production/sculpt height")
	if want_max:
		return _bound(maxf(float(a.get("min", 0.0)), float(b.get("min", 0.0))),
			maxf(float(a.get("max", 0.0)), float(b.get("max", 0.0))))
	return _bound(minf(float(a.get("min", 0.0)), float(b.get("min", 0.0))),
		minf(float(a.get("max", 0.0)), float(b.get("max", 0.0))))


func _abs_bound(value: Dictionary) -> Dictionary:
	if not _is_pure(value):
		return _unknown_bound("Absolute Value depends on absolute production/sculpt height")
	var lo: float = float(value.get("min", 0.0))
	var hi: float = float(value.get("max", 0.0))
	var maximum: float = maxf(absf(lo), absf(hi))
	var minimum: float = 0.0 if lo <= 0.0 and hi >= 0.0 else minf(absf(lo), absf(hi))
	return _bound(minimum, maximum)


func _power_bound(a: Dictionary, b: Dictionary) -> Dictionary:
	if not _is_pure(a) or not _is_exact_scalar(b):
		return _unknown_bound("Power requires a bounded value and constant exponent")
	var exponent: float = float(b.get("min", 0.0))
	if exponent < 0.0:
		return _unknown_bound("Negative power exponent can become unbounded near zero")
	var lo: float = float(a.get("min", 0.0))
	var hi: float = float(a.get("max", 0.0))
	var maximum_abs: float = pow(maxf(absf(lo), absf(hi)), exponent)
	if lo < 0.0 and hi > 0.0:
		return _bound(-maximum_abs, maximum_abs)
	if hi <= 0.0:
		return _bound(-maximum_abs, -pow(minf(absf(lo), absf(hi)), exponent))
	return _bound(pow(minf(absf(lo), absf(hi)), exponent), maximum_abs)


func _clamp_bound(value: Dictionary, edge_a: Dictionary, edge_b: Dictionary) -> Dictionary:
	if not _all_pure([value, edge_a, edge_b]):
		return _unknown_bound("Clamp depends on absolute production/sculpt height")
	return _bound(
		minf(float(value.get("min", 0.0)), minf(float(edge_a.get("min", 0.0)), float(edge_b.get("min", 0.0)))),
		maxf(float(value.get("max", 0.0)), maxf(float(edge_a.get("max", 0.0)), float(edge_b.get("max", 0.0)))))


func _remap_bound(value: Dictionary, in_min: Dictionary, in_max: Dictionary,
		out_min: float, out_max: float) -> Dictionary:
	if not _is_pure(value) or not _is_exact_scalar(in_min) or not _is_exact_scalar(in_max):
		return _unknown_bound("Remap input range must be constant to derive a safe displacement bound")
	var a: float = float(in_min.get("min", 0.0))
	var b: float = float(in_max.get("min", 0.0))
	if absf(b - a) <= 0.000000000001:
		return _bound(out_min, out_min)
	var t0: float = (float(value.get("min", 0.0)) - a) / (b - a)
	var t1: float = (float(value.get("max", 0.0)) - a) / (b - a)
	var y0: float = lerpf(out_min, out_max, t0)
	var y1: float = lerpf(out_min, out_max, t1)
	return _bound(minf(y0, y1), maxf(y0, y1))


func _mix_bound(a: Dictionary, b: Dictionary, factor: Dictionary) -> Dictionary:
	if not _all_pure([a, b, factor]):
		return _unknown_bound("Terrain Blend depends on absolute production/sculpt height")
	return _bound(minf(float(a.get("min", 0.0)), float(b.get("min", 0.0))),
		maxf(float(a.get("max", 0.0)), float(b.get("max", 0.0))))


func _masked_bound(value: Dictionary) -> Dictionary:
	if not _is_pure(value):
		return _unknown_bound("A biome/LOD mask conditionally changes absolute production/sculpt height")
	return _bound(minf(float(value.get("min", 0.0)), 0.0),
		maxf(float(value.get("max", 0.0)), 0.0))


func _add_interval_to_affine(value: Dictionary, interval_min: float,
		interval_max: float) -> Dictionary:
	if not bool(value.get("known", false)):
		return value.duplicate(true)
	return _bound(float(value.get("min", 0.0)) + interval_min,
		float(value.get("max", 0.0)) + interval_max,
		float(value.get("base", 0.0)), float(value.get("sculpt", 0.0)))
