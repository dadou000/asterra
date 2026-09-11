class_name HydroReachabilityService
extends RefCounted
## Production boundary policy for sparse hydrology frontier activation.
##
## Topology only says which tile is adjacent. This service decides whether the
## current source free surface can overtop the destination boundary terrain/crest.
## GroundHeightStore.sample_height() is the default sampler, so sparse Deltas are
## included. An optional structure-crest provider can raise individual samples for
## levees, dams, walls, gates, etc.
##
## The GPU frontier entry supplies source_surface_m=max(h+bed) and source_bed_m
## (the bed under that free surface) on the emitting edge. We sample the smooth
## macro crest on both the source and destination edges and permit activation only
## if the real SWE water column plus the macro slope clears the boundary -- the
## SWE bed's procedural detail cancels out of that difference. The connected SWE
## solver remains the final cell-level hydrostatic gate after allocation, so this
## policy is allowed to be slightly permissive but must never invent hydraulic head.

var atlas: SparseHydroAtlasGPU
var terrain_level := 0
var boundary_samples := 8
var minimum_overtop_head_m := 0.01
## The default terrain sampler only sees the smooth macro/coast field -- it has no
## erosion detail. When the frontier reports the source-edge SWE bed we strip that
## detail bias exactly (see evaluate()); this residual slack only absorbs macro
## bicubic disagreement and sub-tile detail locality (~metres). The SWE solver is
## still the final hydrostatic gate and will not route water uphill.
var macro_crest_slack_m := 2.0
## Mean-sea datum (terrain_height == 0). A destination edge entirely below this
## is the sea floor; the frontier stops there and the SWE coast-drain sink
## (SparseHydroStepGPU.coast_drain_rate_per_s) discharges the river into the ocean.
var sea_level_m := 0.0
## When the GPU frontier reports actual advective discharge (not just predictive
## dry-neighbour wetting) at or above this rate, the SWE is already pushing water
## across that edge -- accept it and let the connected solver be the gate.
var solver_flux_override_m3s := 0.25

var _terrain_sampler := Callable()
var _structure_crest_provider := Callable()
var _evaluations := 0
var _accepted := 0
var _blocked := 0
var _last: Dictionary = {}


## terrain_sampler contract: float(direction: Vector3, terrain_level: int)
## structure_crest_provider contract:
##   Variant(direction, source_key, destination_key, destination_direction, link)
## Return a finite elevation in metres to raise the local crest, or null/non-finite
## when no structure contributes at that sample.
func initialize(p_atlas: SparseHydroAtlasGPU,
		terrain_sampler: Callable = Callable(),
		structure_crest_provider: Callable = Callable()) -> Error:
	if p_atlas == null or not p_atlas.initialized_ok():
		return ERR_INVALID_PARAMETER
	atlas = p_atlas
	boundary_samples = clampi(p_atlas.tile_resolution, 4, 64)
	if terrain_sampler.is_valid():
		_terrain_sampler = terrain_sampler
	else:
		_terrain_sampler = Callable(self, &"_sample_ground_height")
	_structure_crest_provider = structure_crest_provider
	if Planet.ready_state and Planet.cfg != null:
		terrain_level = GroundHeightStore.level_for_spacing(p_atlas.cell_size_m)
	else:
		terrain_level = 0
	return OK


func set_structure_crest_provider(provider: Callable) -> void:
	_structure_crest_provider = provider


func can_enter(source: HydroTileKey, source_direction: int,
		destination: HydroTileKey, flux_m3s: float, link: Dictionary) -> bool:
	return bool(evaluate(source, source_direction, destination, flux_m3s, link)
		.get("reachable", false))


func evaluate(source: HydroTileKey, source_direction: int,
		destination: HydroTileKey, flux_m3s: float, link: Dictionary) -> Dictionary:
	_evaluations += 1
	var result := {
		"reachable": false,
		"reason": "",
		"source_surface_m": -INF,
		"minimum_crest_m": INF,
		"overtopping_head_m": -INF,
		"samples": 0,
		"source_direction": source_direction,
		"destination_direction": int(link.get("destination_direction", -1)),
		"crossed_face": bool(link.get("crossed_face", false)),
	}
	if atlas == null or source == null or destination == null or flux_m3s <= 0.0:
		return _finish_result(result, "invalid_input")
	if not _terrain_sampler.is_valid():
		return _finish_result(result, "no_terrain_sampler")

	var eta := float(link.get("source_surface_m", -INF))
	result["source_surface_m"] = eta
	if not is_finite(eta):
		return _finish_result(result, "missing_source_head")

	var destination_direction := int(link.get("destination_direction", -1))
	if destination_direction < HydroTileTopology.DIR_WEST \
			or destination_direction > HydroTileTopology.DIR_NORTH:
		return _finish_result(result, "invalid_destination_edge")

	var sample_count := clampi(boundary_samples, 1, 256)
	var minimum_crest := INF
	var maximum_crest := -INF
	var valid_samples := 0
	for k in sample_count:
		var t := -1.0 + 2.0 * (float(k) + 0.5) / float(sample_count)
		var d := _edge_direction(destination, destination_direction, t)
		if d.length_squared() < 0.5:
			continue
		var bed_value: Variant = _terrain_sampler.call(d, terrain_level)
		if not (bed_value is float or bed_value is int):
			continue
		var crest := float(bed_value)
		if not is_finite(crest):
			continue

		if _structure_crest_provider.is_valid():
			var structure_value: Variant = _structure_crest_provider.call(
				d, source, destination, destination_direction, link)
			if structure_value is float or structure_value is int:
				var structure_crest := float(structure_value)
				if is_finite(structure_crest):
					crest = maxf(crest, structure_crest)
		minimum_crest = minf(minimum_crest, crest)
		maximum_crest = maxf(maximum_crest, crest)
		valid_samples += 1

	result["samples"] = valid_samples
	result["minimum_crest_m"] = minimum_crest
	if valid_samples <= 0 or not is_finite(minimum_crest):
		return _finish_result(result, "terrain_unavailable")

	# Seaward boundary: if the whole destination edge lies below mean sea level it
	# is sea floor, not a tile the sparse system should own -- the connected SWE
	# coast-drain sink handles discharge into the ocean at the last land tile.
	if is_finite(maximum_crest) and maximum_crest <= sea_level_m:
		return _finish_result(result, "seaward_boundary")

	var head := eta - minimum_crest
	result["overtopping_head_m"] = head

	# Preferred gate. The GPU SWE bed carries the full procedural-detail spectrum
	# that the smooth macro crest sampler cannot see -- locally that is tens of
	# metres, so eta-vs-macro_crest is apples to oranges and blocks real channels.
	# When the frontier reports the source-edge bed, compare on the shared macro
	# datum using the *real* SWE water column:
	#   datum_head = swe_depth + (macro_at_source_edge - macro_at_destination_edge)
	# Both terms are measured, so this never invents hydraulic head, and the
	# connected SWE solver remains the final cell-level hydrostatic gate.
	var source_bed_m := float(link.get("source_bed_m", -INF))
	if is_finite(source_bed_m) and source_bed_m > -1.0e29:
		var source_macro := _sample_macro_edge(source, source_direction)
		if is_finite(source_macro):
			var swe_depth := maxf(eta - source_bed_m, 0.0)
			var datum_head := swe_depth + (source_macro - minimum_crest)
			result["swe_depth_m"] = swe_depth
			result["macro_datum_head_m"] = datum_head
			result["overtopping_head_m"] = datum_head
			# A wet emitting edge carries a real water column, so the full slack is
			# justified. A predictive (dry) edge has no column to lean on -- then
			# only the macro slope decides, with slack cut to crest-sampling noise
			# so a genuine adverse macro step is still blocked.
			var slack := macro_crest_slack_m
			if swe_depth <= 0.02:
				slack = minf(macro_crest_slack_m, 0.75)
			if datum_head + slack + 1.0e-9 >= maxf(minimum_overtop_head_m, 0.0):
				result["reachable"] = true
				return _finish_result(result, "macro_datum_head")
			# Real (non-predictive) discharge already crossing = real moving water.
			if not bool(link.get("predictive_wetting", false)) \
					and flux_m3s >= maxf(solver_flux_override_m3s, 0.0):
				result["reachable"] = true
				return _finish_result(result, "solver_flux_override")
			return _finish_result(result, "insufficient_head")

	if head + 1.0e-9 >= maxf(minimum_overtop_head_m, 0.0):
		result["reachable"] = true
		return _finish_result(result, "overtops_boundary")

	# The macro crest sampler has no erosion detail; give it a bounded slack.
	if head + macro_crest_slack_m + 1.0e-9 >= maxf(minimum_overtop_head_m, 0.0):
		result["reachable"] = true
		return _finish_result(result, "overtops_boundary_macro_slack")

	# The SWE is already discharging real (non-predictive) water across this edge.
	if not bool(link.get("predictive_wetting", false)) \
			and flux_m3s >= maxf(solver_flux_override_m3s, 0.0):
		result["reachable"] = true
		return _finish_result(result, "solver_flux_override")

	return _finish_result(result, "insufficient_head")


func last_evaluation() -> Dictionary:
	return _last.duplicate(true)


func stats() -> Dictionary:
	return {
		"terrain_level": terrain_level,
		"boundary_samples": boundary_samples,
		"minimum_overtop_head_m": minimum_overtop_head_m,
		"evaluations": _evaluations,
		"accepted": _accepted,
		"blocked": _blocked,
		"last": last_evaluation(),
	}


func _finish_result(result: Dictionary, reason: String) -> Dictionary:
	result["reason"] = reason
	if bool(result.get("reachable", false)):
		_accepted += 1
	else:
		_blocked += 1
	_last = result.duplicate(true)
	return result


func _sample_ground_height(direction: Vector3, level: int) -> float:
	if not Planet.ready_state or Planet.cfg == null:
		return NAN
	return GroundHeightStore.sample_height(direction, level)


## Lowest smooth-macro elevation along the source tile's emitting edge, sampled
## with the same terrain sampler used for the destination crest so the datum is
## shared. Returns NAN when nothing finite was sampled.
func _sample_macro_edge(key: HydroTileKey, direction: int) -> float:
	if not _terrain_sampler.is_valid() or key == null:
		return NAN
	if direction < HydroTileTopology.DIR_WEST \
			or direction > HydroTileTopology.DIR_NORTH:
		return NAN
	var lowest := INF
	var seen := 0
	var count := clampi(boundary_samples, 1, 256)
	for k in count:
		var t := -1.0 + 2.0 * (float(k) + 0.5) / float(count)
		var d := _edge_direction(key, direction, t)
		if d.length_squared() < 0.5:
			continue
		var value: Variant = _terrain_sampler.call(d, terrain_level)
		if not (value is float or value is int):
			continue
		var h := float(value)
		if not is_finite(h):
			continue
		lowest = minf(lowest, h)
		seen += 1
	return lowest if seen > 0 else NAN


func _edge_direction(key: HydroTileKey, direction: int, t: float) -> Vector3:
	var bounds := HydroTileTopology.tile_bounds_face_uv(key)
	var p := bounds.position + bounds.size * 0.5
	var s := clampf(t, -1.0, 1.0)
	match direction:
		HydroTileTopology.DIR_WEST:
			p = Vector2(bounds.position.x,
				bounds.position.y + (s * 0.5 + 0.5) * bounds.size.y)
		HydroTileTopology.DIR_EAST:
			p = Vector2(bounds.end.x,
				bounds.position.y + (s * 0.5 + 0.5) * bounds.size.y)
		HydroTileTopology.DIR_SOUTH:
			p = Vector2(bounds.position.x + (s * 0.5 + 0.5) * bounds.size.x,
				bounds.position.y)
		HydroTileTopology.DIR_NORTH:
			p = Vector2(bounds.position.x + (s * 0.5 + 0.5) * bounds.size.x,
				bounds.end.y)
		_:
			return Vector3.ZERO
	return CubeSphere.face_uv_to_dir(key.face, p.x, p.y)
