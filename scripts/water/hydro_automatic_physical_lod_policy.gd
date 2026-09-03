class_name HydroAutomaticPhysicalLODPolicy
extends RefCounted
## Automatic physical HydroLOD policy for the sparse 2-D hydrology atlas.
##
## This object owns *policy only*. It never allocates/replaces tiles directly;
## every returned action must pass through HydroPhysicalLODManager and therefore
## inherits the transactional transfer, 2:1 topology and river/component guards.
##
## Refinement priority:
##   explicit gameplay/detail hold -> invalid state -> player focus -> dynamic flow
##
## Coarsening requires all four siblings to be quiet for a hysteresis interval.
## Capacity pressure shortens that interval but never overrides activity, explicit
## detail holds, player-detail requirements or the normal transition eligibility
## guard. One action is emitted per completed hydrology cycle.

var enabled := true
var maximum_physical_lod := 4

# Geodesic player/focus detail rings. Entry i requests at least Hi inside radius i.
var focus_radius_m := PackedFloat32Array([2500.0, 10000.0, 40000.0, 120000.0])

# Resolution-independent refinement thresholds.
var refine_velocity_high_mps := 0.75
var refine_velocity_medium_mps := 0.15
var refine_discharge_per_width_high_m2s := 0.05
var refine_discharge_per_width_medium_m2s := 0.01
var refine_energy_density_high := 0.05
var refine_energy_density_medium := 0.01

# Exit band for coarsening. These are deliberately below the refinement band.
var coarsen_velocity_max_mps := 0.03
var coarsen_discharge_per_width_max_m2s := 0.002
var coarsen_predictive_per_width_max_m2s := 0.002
var coarsen_energy_density_max := 0.002
var coarsen_quiet_s := 12.0
var pressure_quiet_s := 3.0
var transition_cooldown_s := 5.0
var retry_cooldown_s := 1.5

var capacity_pressure_enter := 0.82
var capacity_pressure_emergency := 0.94

var scheduler: SparseHydroScheduler
var atlas: SparseHydroAtlasGPU

var _policy_time_s := 0.0
var _quiet_seconds: Dictionary = {} # resident tile id -> seconds below exit band
var _cooldown_until: Dictionary = {} # stable tile id -> policy time
var _forced_targets: Dictionary = {} # requested location id -> {target_lod, until_s}

var _cycles := 0
var _actions_started := 0
var _refinements_started := 0
var _coarsens_started := 0
var _balance_actions := 0
var _capacity_actions := 0
var _eligibility_rejections := 0
var _focus_refinements := 0
var _dynamic_refinements := 0
var _forced_refinements := 0
var _last_action: Dictionary = {}
var _last_reason := "idle"
var _last_pressure := 0.0
var _last_candidate_counts := {"refine": 0, "coarsen": 0}


func initialize(p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU,
		p_maximum_physical_lod: int = 4) -> Error:
	if p_scheduler == null or p_scheduler.pool == null or p_atlas == null \
			or not p_atlas.initialized_ok() or not p_atlas.hydrolod_enabled():
		return ERR_INVALID_PARAMETER
	if p_scheduler.pool.capacity != p_atlas.capacity:
		return ERR_INVALID_PARAMETER
	scheduler = p_scheduler
	atlas = p_atlas
	maximum_physical_lod = clampi(p_maximum_physical_lod, 0, 7)
	return OK


func initialized_ok() -> bool:
	return scheduler != null and scheduler.pool != null and atlas != null \
		and atlas.initialized_ok()


## Request temporary detail around a stable tile location. The requested tile need
## not currently be resident: a covering coarse owner is refined toward target_lod.
func request_detail(tile_id: int, target_lod: int = 0, hold_s: float = 5.0) -> Error:
	if not initialized_ok() or tile_id < 0 or not is_finite(hold_s) or hold_s < 0.0:
		return ERR_INVALID_PARAMETER
	var key := HydroTileKey.unpack(tile_id)
	if key == null or not HydroLODHierarchy.valid_physical_key(
			atlas.base_tile_level, maximum_physical_lod, key):
		return ERR_INVALID_PARAMETER
	_forced_targets[tile_id] = {
		"target_lod": clampi(target_lod, 0, maximum_physical_lod),
		"until_s": _policy_time_s + hold_s,
	}
	return OK


func clear_detail_request(tile_id: int) -> bool:
	return _forced_targets.erase(tile_id)


func clear_detail_requests() -> void:
	_forced_targets.clear()


## Called once after a completed hydrology cycle while the runtime is IDLE.
## Eligibility callables must return HydroPhysicalLODManager's eligibility dictionary.
func choose_action(summaries: Array[Dictionary], advanced_dt_s: float,
		focus_context: Dictionary, refine_eligibility: Callable,
		coarsen_eligibility: Callable) -> Dictionary:
	if not initialized_ok() or not refine_eligibility.is_valid() \
			or not coarsen_eligibility.is_valid():
		_last_reason = "unconfigured"
		return {}
	_cycles += 1
	var cycle_dt := maxf(advanced_dt_s, 0.0) if is_finite(advanced_dt_s) else 0.0
	_policy_time_s += cycle_dt
	_prune_expired_state()

	var summary_by_id := _summary_map(summaries)
	_update_quiet_timers(summary_by_id, cycle_dt)
	_last_pressure = clampf(scheduler.capacity_pressure(), 0.0, 1.0)
	_last_candidate_counts = {"refine": 0, "coarsen": 0}
	if not enabled:
		_last_reason = "disabled"
		return {}

	var focus_direction := focus_context.get("direction", Vector3.ZERO) as Vector3
	if focus_direction.length_squared() > 1.0e-12:
		focus_direction = focus_direction.normalized()
	else:
		focus_direction = Vector3.ZERO
	var planet_radius_m := maxf(float(focus_context.get("planet_radius_m", 0.0)), 0.0)

	# Detail/safety always wins over normal coarsening. If refinement cannot start
	# solely because the atlas is full, the later pressure pass can first free 3 slots.
	var refine := _best_refinement(summary_by_id, focus_direction, planet_radius_m,
		refine_eligibility)
	if not refine.is_empty():
		_last_reason = String(refine.get("reason", "refine"))
		return refine

	var coarsen := _best_coarsening(summary_by_id, focus_direction, planet_radius_m,
		coarsen_eligibility, _last_pressure)
	if not coarsen.is_empty():
		_last_reason = String(coarsen.get("reason", "coarsen"))
		return coarsen

	_last_reason = "no_eligible_transition"
	return {}


func note_action_started(action: Dictionary) -> void:
	if action.is_empty():
		return
	_actions_started += 1
	var mode := String(action.get("mode", ""))
	var parent := action.get("parent") as HydroTileKey
	if mode == "refine":
		_refinements_started += 1
	elif mode == "coarsen":
		_coarsens_started += 1
	if bool(action.get("balance", false)):
		_balance_actions += 1
	if bool(action.get("capacity_pressure", false)):
		_capacity_actions += 1
	match String(action.get("reason", "")):
		"focus_refine": _focus_refinements += 1
		"dynamic_refine", "invalid_state_refine": _dynamic_refinements += 1
		"forced_detail_refine": _forced_refinements += 1
	_mark_cooldown(parent, transition_cooldown_s)
	_last_action = _action_snapshot(action)


func note_transition_completed(report: Dictionary) -> void:
	var parent_id := int(report.get("parent_tile_id", -1))
	if parent_id >= 0:
		var parent := HydroTileKey.unpack(parent_id)
		_mark_cooldown(parent, transition_cooldown_s)
		_quiet_seconds.erase(parent_id)
		for child in HydroLODHierarchy.children(parent):
			_quiet_seconds.erase(child.packed())
	_last_reason = String(report.get("mode", "transition")) + "_completed"


func note_transition_failed(_error: Error, stage: String, _recovery: String) -> void:
	var parent_value: Variant = _last_action.get("parent_tile_id", -1)
	var parent_id := int(parent_value)
	if parent_id >= 0:
		_mark_cooldown(HydroTileKey.unpack(parent_id), retry_cooldown_s)
	_last_reason = "transition_failed_" + stage


func stats() -> Dictionary:
	return {
		"initialized": initialized_ok(),
		"enabled": enabled,
		"maximum_physical_lod": maximum_physical_lod,
		"cycles": _cycles,
		"actions_started": _actions_started,
		"refinements_started": _refinements_started,
		"coarsens_started": _coarsens_started,
		"balance_actions": _balance_actions,
		"capacity_actions": _capacity_actions,
		"eligibility_rejections": _eligibility_rejections,
		"focus_refinements": _focus_refinements,
		"dynamic_refinements": _dynamic_refinements,
		"forced_refinements": _forced_refinements,
		"capacity_pressure": _last_pressure,
		"capacity_pressure_enter": capacity_pressure_enter,
		"capacity_pressure_emergency": capacity_pressure_emergency,
		"quiet_tiles": _quiet_seconds.size(),
		"forced_detail_holds": _forced_targets.size(),
		"cooldowns": _cooldown_until.size(),
		"last_candidate_counts": _last_candidate_counts.duplicate(true),
		"last_reason": _last_reason,
		"last_action": _last_action.duplicate(true),
		"player_focus_camera_independent": true,
		"one_transition_per_cycle": true,
		"uses_transactional_manager": true,
		"two_to_one_balance_propagation": true,
	}


func _best_refinement(summary_by_id: Dictionary, focus_direction: Vector3,
		planet_radius_m: float, refine_eligibility: Callable) -> Dictionary:
	var ranked: Array[Dictionary] = []

	# Explicit gameplay/structure detail holds address a physical location rather
	# than a particular current representation.
	for requested_id_value: Variant in _forced_targets.keys():
		var requested_id := int(requested_id_value)
		var requested_key := HydroTileKey.unpack(requested_id)
		var covering := HydroLODHierarchy.covering_record(scheduler.pool, requested_key)
		if covering.is_empty():
			continue
		var owner := covering.get("key") as HydroTileKey
		if owner == null:
			continue
		var owner_lod := atlas.physical_lod_for_level(owner.level)
		var forced: Dictionary = _forced_targets[requested_id]
		var target := clampi(int(forced.get("target_lod", 0)), 0, maximum_physical_lod)
		if owner_lod <= target:
			continue
		ranked.append({
			"parent": owner,
			"target_lod": target,
			"score": 30000.0 + float(owner_lod - target) * 100.0,
			"reason": "forced_detail_refine",
			"urgent": true,
		})

	for record in scheduler.pool.active_records():
		if int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
				== HydroTilePool.TileState.ALLOCATING:
			continue
		var key := record.get("key") as HydroTileKey
		if key == null:
			continue
		var lod := atlas.physical_lod_for_level(key.level)
		if lod <= 0 or lod > maximum_physical_lod:
			continue
		var tile_id := key.packed()
		var summary_value: Variant = summary_by_id.get(tile_id, null)
		if not (summary_value is Dictionary):
			continue
		var summary := summary_value as Dictionary
		var target := _dynamic_target_lod(summary, key)
		var reason := "dynamic_refine"
		var score := _dynamic_refine_score(summary, key)
		var urgent := int(summary.get("invalid_cells", 0)) > 0
		if urgent:
			target = 0
			reason = "invalid_state_refine"
			score += 20000.0

		var focus_target := _focus_target_lod(key, focus_direction, planet_radius_m)
		if focus_target >= 0 and (target < 0 or focus_target < target):
			target = focus_target
			reason = "focus_refine"
			score = maxf(score, 10000.0 + float(lod - focus_target) * 100.0)
			urgent = focus_target == 0
		if target < 0 or lod <= target:
			continue
		if not urgent and _cooldown_active(key):
			continue
		ranked.append({
			"parent": key,
			"target_lod": target,
			"score": score + float(lod - target) * 10.0,
			"reason": reason,
			"urgent": urgent,
		})

	_last_candidate_counts["refine"] = ranked.size()
	ranked.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("score", 0.0)) > float(b.get("score", 0.0)))

	for candidate in ranked:
		var key := candidate.get("parent") as HydroTileKey
		if key == null or not scheduler.pool.contains(key):
			continue
		var prerequisite := _refine_balance_prerequisite(key, refine_eligibility)
		if prerequisite != null:
			return {
				"mode": "refine",
				"parent": prerequisite,
				"reason": "balance_refine",
				"score": float(candidate.get("score", 0.0)) + 500.0,
				"target_lod": atlas.physical_lod_for_level(prerequisite.level + 1),
				"balance": true,
			}
		if _eligible(refine_eligibility, key):
			var out := candidate.duplicate(true)
			out["mode"] = "refine"
			out["balance"] = false
			return out
		_eligibility_rejections += 1
	return {}


func _best_coarsening(summary_by_id: Dictionary, focus_direction: Vector3,
		planet_radius_m: float, coarsen_eligibility: Callable,
		pressure: float) -> Dictionary:
	var parents: Dictionary = {}
	for record in scheduler.pool.active_records():
		if int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
				== HydroTilePool.TileState.ALLOCATING:
			continue
		var child := record.get("key") as HydroTileKey
		if child == null or child.level <= 0:
			continue
		var parent := child.parent()
		var parent_lod := atlas.physical_lod_for_level(parent.level)
		if parent_lod <= 0 or parent_lod > maximum_physical_lod:
			continue
		parents[parent.packed()] = parent

	var pressure_factor := 0.0
	if pressure > capacity_pressure_enter:
		pressure_factor = clampf((pressure - capacity_pressure_enter) /
			maxf(capacity_pressure_emergency - capacity_pressure_enter, 1.0e-6), 0.0, 1.0)
	var required_quiet := lerpf(coarsen_quiet_s, pressure_quiet_s, pressure_factor)
	var ranked: Array[Dictionary] = []

	for parent_value: Variant in parents.values():
		var parent := parent_value as HydroTileKey
		if parent == null or _cooldown_active(parent) \
				or _forced_detail_blocks_coarsen(parent):
			continue
		var resident := HydroLODHierarchy.immediate_children_resident(
			scheduler.pool, parent, true)
		if not bool(resident.get("ready", false)):
			continue
		var children := resident.get("children", []) as Array[HydroTileKey]
		var quiet_min := INF
		var safe := true
		for child in children:
			var summary_value: Variant = summary_by_id.get(child.packed(), null)
			if not (summary_value is Dictionary) \
					or not _summary_quiet(summary_value as Dictionary, child):
				safe = false
				break
			quiet_min = minf(quiet_min, float(_quiet_seconds.get(child.packed(), 0.0)))
		if not safe or quiet_min < required_quiet:
			continue

		var parent_lod := atlas.physical_lod_for_level(parent.level)
		var focus_target := _focus_target_lod(parent, focus_direction, planet_radius_m)
		if focus_target >= 0 and parent_lod > focus_target:
			continue
		var focus_distance := _focus_distance_m(parent, focus_direction, planet_radius_m)
		var distance_bonus := 0.0 if not is_finite(focus_distance) \
			else minf(focus_distance / 10000.0, 50.0)
		# Preserve very coarse detail when equivalent capacity can be reclaimed by
		# combining a finer sibling quartet instead.
		var score := quiet_min + distance_bonus - float(parent_lod) * 12.0 \
			+ pressure_factor * 100.0
		ranked.append({
			"parent": parent,
			"score": score,
			"quiet_s": quiet_min,
			"reason": "capacity_coarsen" if pressure_factor > 0.0 else "quiet_coarsen",
			"capacity_pressure": pressure_factor > 0.0,
		})

	_last_candidate_counts["coarsen"] = ranked.size()
	ranked.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("score", 0.0)) > float(b.get("score", 0.0)))

	for candidate in ranked:
		var parent := candidate.get("parent") as HydroTileKey
		if parent == null:
			continue
		var prerequisite := _coarsen_balance_prerequisite(parent, summary_by_id,
			focus_direction, planet_radius_m, required_quiet, coarsen_eligibility)
		if prerequisite != null:
			return {
				"mode": "coarsen",
				"parent": prerequisite,
				"reason": "balance_coarsen",
				"score": float(candidate.get("score", 0.0)) + 500.0,
				"balance": true,
				"capacity_pressure": bool(candidate.get("capacity_pressure", false)),
			}
		if _eligible(coarsen_eligibility, parent):
			var out := candidate.duplicate(true)
			out["mode"] = "coarsen"
			out["balance"] = false
			return out
		_eligibility_rejections += 1
	return {}


func _refine_balance_prerequisite(parent: HydroTileKey,
		refine_eligibility: Callable) -> HydroTileKey:
	# Refining parent produces children at parent.level+1. A neighboring covering
	# owner at parent.level-1 would therefore become a 2-level jump; refine it first.
	for direction in 4:
		var link := HydroTileTopology.neighbor(parent, direction)
		if link.is_empty():
			continue
		var region := link.get("key") as HydroTileKey
		if region == null:
			continue
		var covering := HydroLODHierarchy.covering_record(scheduler.pool, region)
		if covering.is_empty():
			continue
		var key := covering.get("key") as HydroTileKey
		if key == null or key.level >= parent.level:
			continue
		if _eligible(refine_eligibility, key):
			return key
	return null


func _coarsen_balance_prerequisite(parent: HydroTileKey, summary_by_id: Dictionary,
		focus_direction: Vector3, planet_radius_m: float, required_quiet: float,
		coarsen_eligibility: Callable) -> HydroTileKey:
	# Coarsening to parent.level cannot border a leaf deeper than parent.level+1.
	# Collapse the deepest touching sibling quartet by one level first.
	var deepest: HydroTileKey
	for direction in 4:
		var link := HydroTileTopology.neighbor(parent, direction)
		if link.is_empty():
			continue
		var region := link.get("key") as HydroTileKey
		var destination_direction := int(link.get("destination_direction", -1))
		if region == null or destination_direction < 0:
			continue
		for record in HydroLODHierarchy.descendant_records(scheduler.pool, region):
			var key := record.get("key") as HydroTileKey
			if key == null or key.level <= parent.level + 1 \
					or not _touches_region_edge(key, region, destination_direction):
				continue
			if deepest == null or key.level > deepest.level:
				deepest = key
	if deepest == null:
		return null
	var prerequisite := deepest.parent()
	if prerequisite == null or not _coarsen_group_safe(prerequisite, summary_by_id,
			focus_direction, planet_radius_m, required_quiet):
		return null
	return prerequisite if _eligible(coarsen_eligibility, prerequisite) else null


func _coarsen_group_safe(parent: HydroTileKey, summary_by_id: Dictionary,
		focus_direction: Vector3, planet_radius_m: float, required_quiet: float) -> bool:
	if parent == null or _cooldown_active(parent) or _forced_detail_blocks_coarsen(parent):
		return false
	var resident := HydroLODHierarchy.immediate_children_resident(scheduler.pool, parent, true)
	if not bool(resident.get("ready", false)):
		return false
	var parent_lod := atlas.physical_lod_for_level(parent.level)
	var focus_target := _focus_target_lod(parent, focus_direction, planet_radius_m)
	if focus_target >= 0 and parent_lod > focus_target:
		return false
	var children := resident.get("children", []) as Array[HydroTileKey]
	for child in children:
		var value: Variant = summary_by_id.get(child.packed(), null)
		if not (value is Dictionary) or not _summary_quiet(value as Dictionary, child) \
				or float(_quiet_seconds.get(child.packed(), 0.0)) < required_quiet:
			return false
	return true


func _dynamic_target_lod(summary: Dictionary, key: HydroTileKey) -> int:
	var velocity := maxf(float(summary.get("max_velocity_mps", 0.0)), 0.0)
	var q_width := _max_flux_per_width(summary, key, false)
	var energy_density := _energy_density(summary, key)
	if velocity >= refine_velocity_high_mps \
			or q_width >= refine_discharge_per_width_high_m2s \
			or energy_density >= refine_energy_density_high:
		return 0
	if velocity >= refine_velocity_medium_mps \
			or q_width >= refine_discharge_per_width_medium_m2s \
			or energy_density >= refine_energy_density_medium:
		return mini(1, maximum_physical_lod)
	return -1


func _dynamic_refine_score(summary: Dictionary, key: HydroTileKey) -> float:
	var velocity_ratio := maxf(float(summary.get("max_velocity_mps", 0.0)), 0.0) \
		/ maxf(refine_velocity_medium_mps, 1.0e-6)
	var q_ratio := _max_flux_per_width(summary, key, false) \
		/ maxf(refine_discharge_per_width_medium_m2s, 1.0e-8)
	var energy_ratio := _energy_density(summary, key) \
		/ maxf(refine_energy_density_medium, 1.0e-8)
	return maxf(velocity_ratio, maxf(q_ratio, energy_ratio)) * 100.0


func _summary_quiet(summary: Dictionary, key: HydroTileKey) -> bool:
	if int(summary.get("invalid_cells", 0)) > 0:
		return false
	return maxf(float(summary.get("max_velocity_mps", 0.0)), 0.0) \
			<= coarsen_velocity_max_mps \
		and _max_flux_per_width(summary, key, false) \
			<= coarsen_discharge_per_width_max_m2s \
		and _max_flux_per_width(summary, key, true) \
			<= coarsen_predictive_per_width_max_m2s \
		and _energy_density(summary, key) <= coarsen_energy_density_max


func _max_flux_per_width(summary: Dictionary, key: HydroTileKey,
		predictive: bool) -> float:
	var prefix := "wetting_" if predictive else "flux_"
	var maximum := 0.0
	for direction in ["west", "east", "south", "north"]:
		maximum = maxf(maximum, maxf(float(summary.get(
			prefix + direction + "_m3s", 0.0)), 0.0))
	var width := maxf(atlas.tile_width_for_level(key.level), 1.0e-6)
	return maximum / width


func _energy_density(summary: Dictionary, key: HydroTileKey) -> float:
	var wet_cells := maxi(int(summary.get("wet_cells", 0)), 0)
	if wet_cells <= 0:
		return 0.0
	var wet_area := float(wet_cells) * maxf(atlas.cell_area_for_level(key.level), 1.0e-8)
	return maxf(float(summary.get("kinetic_energy_proxy", 0.0)), 0.0) / wet_area


func _focus_target_lod(key: HydroTileKey, focus_direction: Vector3,
		planet_radius_m: float) -> int:
	var distance := _focus_distance_m(key, focus_direction, planet_radius_m)
	if not is_finite(distance):
		return -1
	var count := mini(focus_radius_m.size(), maximum_physical_lod + 1)
	for lod in count:
		if distance <= float(focus_radius_m[lod]):
			return lod
	return -1


func _focus_distance_m(key: HydroTileKey, focus_direction: Vector3,
		planet_radius_m: float) -> float:
	if key == null or focus_direction.length_squared() <= 1.0e-12 or planet_radius_m <= 0.0:
		return INF
	var uv := HydroTileTopology.tile_center_face_uv(key)
	var center := CubeSphere.face_uv_to_dir(key.face, uv.x, uv.y)
	var dot_value := clampf(center.normalized().dot(focus_direction.normalized()), -1.0, 1.0)
	return acos(dot_value) * planet_radius_m


func _summary_map(summaries: Array[Dictionary]) -> Dictionary:
	var out: Dictionary = {}
	for summary in summaries:
		var tile_id := int(summary.get("tile_id", -1))
		if tile_id >= 0 and scheduler.pool.contains(tile_id):
			out[tile_id] = summary.duplicate(true)
	return out


func _update_quiet_timers(summary_by_id: Dictionary, dt_s: float) -> void:
	var resident: Dictionary = {}
	for record in scheduler.pool.active_records():
		if int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
				== HydroTilePool.TileState.ALLOCATING:
			continue
		var key := record.get("key") as HydroTileKey
		if key == null:
			continue
		var tile_id := key.packed()
		resident[tile_id] = true
		var value: Variant = summary_by_id.get(tile_id, null)
		if value is Dictionary and _summary_quiet(value as Dictionary, key):
			_quiet_seconds[tile_id] = float(_quiet_seconds.get(tile_id, 0.0)) + dt_s
		else:
			_quiet_seconds[tile_id] = 0.0
	for tile_id_value: Variant in _quiet_seconds.keys():
		if not resident.has(int(tile_id_value)):
			_quiet_seconds.erase(tile_id_value)


func _forced_detail_blocks_coarsen(parent: HydroTileKey) -> bool:
	if parent == null:
		return true
	for requested_id_value: Variant in _forced_targets.keys():
		var requested_id := int(requested_id_value)
		var requested := HydroTileKey.unpack(requested_id)
		if requested != null and HydroLODHierarchy.overlaps(parent, requested):
			var target := int((_forced_targets[requested_id] as Dictionary).get("target_lod", 0))
			if atlas.physical_lod_for_level(parent.level) > target:
				return true
	return false


func _eligible(callable: Callable, key: HydroTileKey) -> bool:
	if key == null or not callable.is_valid():
		return false
	var value: Variant = callable.call(key)
	if not (value is Dictionary):
		return false
	var result := value as Dictionary
	return int(result.get("error", FAILED)) == OK and bool(result.get("eligible", false))


func _cooldown_active(key: HydroTileKey) -> bool:
	if key == null:
		return true
	var until := float(_cooldown_until.get(key.packed(), 0.0))
	if _policy_time_s < until:
		return true
	# A recently swapped parent/child representation inherits the same anti-thrash
	# window from its immediate family.
	if key.level > 0 and _policy_time_s < float(_cooldown_until.get(
			key.parent().packed(), 0.0)):
		return true
	for child in HydroLODHierarchy.children(key):
		if _policy_time_s < float(_cooldown_until.get(child.packed(), 0.0)):
			return true
	return false


func _mark_cooldown(parent: HydroTileKey, duration_s: float) -> void:
	if parent == null:
		return
	var until := _policy_time_s + maxf(duration_s, 0.0)
	_cooldown_until[parent.packed()] = maxf(float(_cooldown_until.get(
		parent.packed(), 0.0)), until)
	for child in HydroLODHierarchy.children(parent):
		_cooldown_until[child.packed()] = maxf(float(_cooldown_until.get(
			child.packed(), 0.0)), until)


func _prune_expired_state() -> void:
	for tile_id_value: Variant in _cooldown_until.keys():
		if _policy_time_s >= float(_cooldown_until[tile_id_value]):
			_cooldown_until.erase(tile_id_value)
	for tile_id_value: Variant in _forced_targets.keys():
		var request := _forced_targets[tile_id_value] as Dictionary
		if _policy_time_s >= float(request.get("until_s", 0.0)):
			_forced_targets.erase(tile_id_value)


func _touches_region_edge(candidate: HydroTileKey, region: HydroTileKey,
		direction: int) -> bool:
	if candidate == null or region == null \
			or not HydroLODHierarchy.is_ancestor(region, candidate):
		return false
	var shift := candidate.level - region.level
	var side := 1 << shift
	var local_x := candidate.x - (region.x << shift)
	var local_y := candidate.y - (region.y << shift)
	match direction:
		HydroTileTopology.DIR_WEST: return local_x == 0
		HydroTileTopology.DIR_EAST: return local_x == side - 1
		HydroTileTopology.DIR_SOUTH: return local_y == 0
		HydroTileTopology.DIR_NORTH: return local_y == side - 1
	return false


func _action_snapshot(action: Dictionary) -> Dictionary:
	var out := action.duplicate(true)
	var parent := action.get("parent") as HydroTileKey
	out.erase("parent")
	if parent != null:
		out["parent_tile_id"] = parent.packed()
		out["parent_level"] = parent.level
		out["physical_lod"] = atlas.physical_lod_for_level(parent.level)
	return out
