extends "res://scripts/water/water_system_hydrolod_production.gd"
## Final Phase-4 production facade with temporal + automatic physical HydroLOD.
##
## All river/component ownership, spatial HydroLOD transfer and 2:1 topology logic
## remain inherited. This layer selects SparseHydrologyRuntimeSubcycled and supplies
## a body-fixed local-player focus to its automatic refinement/coarsening policy.

var automatic_physical_hydrolod_enabled := true
var _automatic_hydrolod_focus_override := Vector3.ZERO
var _automatic_hydrolod_focus_override_active := false
var _automatic_hydrolod_focus_source := "none"


func set_automatic_physical_hydrolod_enabled(value: bool) -> Error:
	automatic_physical_hydrolod_enabled = value
	if _sparse_runtime is SparseHydrologyRuntimeSubcycled:
		return (_sparse_runtime as SparseHydrologyRuntimeSubcycled) \
			.set_automatic_hydrolod_enabled(value)
	return OK


func automatic_physical_hydrolod_active() -> bool:
	return _sparse_runtime is SparseHydrologyRuntimeSubcycled \
		and (_sparse_runtime as SparseHydrologyRuntimeSubcycled).automatic_hydrolod_enabled()


## Manual focus override for editor/gameplay systems. Vector3 is an Asterra body-fixed
## direction; clear_automatic_physical_hydrolod_focus() restores local-player focus.
func set_automatic_physical_hydrolod_focus_direction(direction: Vector3) -> Error:
	if direction.length_squared() <= 1.0e-12:
		return ERR_INVALID_PARAMETER
	_automatic_hydrolod_focus_override = direction.normalized()
	_automatic_hydrolod_focus_override_active = true
	return OK


func clear_automatic_physical_hydrolod_focus() -> void:
	_automatic_hydrolod_focus_override = Vector3.ZERO
	_automatic_hydrolod_focus_override_active = false


## Generic disturbance/detail pin. Terrain edits, explosions, construction or other
## gameplay systems can request H0/H1 detail without taking ownership of LOD state.
func request_physical_hydrolod_detail(tile_id: int, target_lod: int = 0,
		hold_s: float = 5.0) -> Error:
	if not (_sparse_runtime is SparseHydrologyRuntimeSubcycled):
		return ERR_UNCONFIGURED
	return (_sparse_runtime as SparseHydrologyRuntimeSubcycled) \
		.request_automatic_hydrolod_detail(tile_id, target_lod, hold_s)


func clear_physical_hydrolod_detail(tile_id: int) -> bool:
	return _sparse_runtime is SparseHydrologyRuntimeSubcycled \
		and (_sparse_runtime as SparseHydrologyRuntimeSubcycled) \
			.clear_automatic_hydrolod_detail(tile_id)


func gpu_stats() -> Dictionary:
	var out := super.gpu_stats()
	var physical: Dictionary = out.get("physical_hydrolod", {})
	var subcycled := _sparse_runtime is SparseHydrologyRuntimeSubcycled
	var cached_cfl := subcycled \
		and (_sparse_runtime as SparseHydrologyRuntimeSubcycled).cfl_cache != null \
		and (_sparse_runtime as SparseHydrologyRuntimeSubcycled).cfl_cache.initialized_ok()
	physical["temporal_subcycling"] = subcycled
	physical["binary_ratios"] = "H0:1,H1:2,H2:4,H3:8,H4:16"
	physical["fine_clock_cfl_normalization"] = true
	physical["coarse_fine_flux_registers"] = true
	physical["synchronizes_at_advance_boundary"] = true
	physical["gpu_due_slot_queue"] = subcycled
	physical["indirect_swe_dispatch"] = subcycled
	physical["indirect_commit_dispatch"] = subcycled
	physical["cpu_due_count_readback"] = false
	physical["non_due_state_copy_eliminated"] = true
	physical["non_due_per_cell_invocations_eliminated"] = true
	physical["cached_cfl_tile_summaries"] = cached_cfl
	physical["fine_tick_cfl_full_cell_scan"] = not cached_cfl
	physical["cfl_cache_topology_revision_invalidation"] = cached_cfl
	physical["cfl_cache_due_only_refresh"] = cached_cfl
	physical["fused_activity_summary_refresh"] = cached_cfl
	physical["activity_summary_reuses_cfl_cell_scan"] = cached_cfl
	physical["post_solve_activity_compute_dispatch"] = not cached_cfl
	physical["automatic_policy_enabled"] = automatic_physical_hydrolod_active()
	physical["automatic_policy_configured"] = automatic_physical_hydrolod_enabled
	physical["automatic_focus_source"] = _automatic_hydrolod_focus_source
	physical["automatic_player_focus"] = true
	physical["automatic_disturbance_detail_api"] = true
	out["physical_hydrolod"] = physical
	return out


func _on_sparse_connectivity_initialized(generation: int,
		connectivity: SparseHydroConnectivityGPU) -> void:
	if not _sparse_generation_matches(generation) or connectivity != _sparse_connectivity:
		return
	var sync_error := connectivity.sync_pool(_sparse_scheduler.pool)
	if sync_error != OK:
		_fail_sparse_bootstrap(sync_error, "connectivity_bootstrap_sync")
		return

	_sparse_reachability = HydroReachabilityService.new()
	var reachability_error := _sparse_reachability.initialize(_sparse_atlas)
	if reachability_error != OK:
		_fail_sparse_bootstrap(reachability_error, "reachability")
		return
	if _structure_crest_provider.is_valid():
		_sparse_reachability.set_structure_crest_provider(_structure_crest_provider)

	var runtime := SparseHydrologyRuntimeSubcycled.new()
	runtime.name = "SparseHydrologyRuntime"
	runtime.process_priority = 12
	runtime.auto_run = true
	runtime.macro_dt_s = maxf(sparse_macro_dt_s, 1.0e-5)
	runtime.max_time_debt_s = maxf(sparse_max_time_debt_s, runtime.macro_dt_s)
	runtime.max_gpu_substeps = clampi(sparse_max_gpu_substeps, 1,
		SparseHydroStepGPU.MAX_GPU_SUBSTEPS)
	runtime.maximum_physical_lod = maximum_physical_hydrolod
	_sparse_runtime = runtime
	add_child(runtime)
	runtime.initialized.connect(func(): _on_sparse_runtime_initialized(generation, runtime))
	runtime.initialization_failed.connect(func(error: Error, component: String):
		_on_sparse_runtime_init_failed(generation, runtime, error, component))
	runtime.runtime_failed.connect(func(error: Error, stage: String):
		_on_sparse_runtime_failed(generation, runtime, error, stage))
	runtime.hydrolod_ready.connect(func():
		if _sparse_generation_matches(generation) and runtime == _sparse_runtime:
			physical_hydrolod_ready.emit())
	runtime.hydrolod_transition_completed.connect(func(report: Dictionary):
		if runtime == _sparse_runtime:
			physical_hydrolod_transition_completed.emit(report.duplicate(true)))
	runtime.hydrolod_transition_failed.connect(
		func(error: Error, stage: String, recovery: String):
			if runtime == _sparse_runtime:
				physical_hydrolod_transition_failed.emit(error, stage, recovery))
	_set_sparse_state("initializing_runtime")
	var runtime_error := runtime.initialize(_sparse_scheduler, _sparse_atlas,
		_sparse_connectivity, _sparse_identity,
		Callable(_sparse_reachability, &"can_enter"))
	if runtime_error != OK:
		_fail_sparse_bootstrap(runtime_error, "runtime_submit")


func _on_sparse_runtime_initialized(generation: int,
		runtime: SparseHydrologyRuntime) -> void:
	if _sparse_generation_matches(generation) and runtime == _sparse_runtime \
			and runtime is SparseHydrologyRuntimeSubcycled:
		var subcycled := runtime as SparseHydrologyRuntimeSubcycled
		var policy_error := subcycled.set_automatic_hydrolod_enabled(
			automatic_physical_hydrolod_enabled)
		if policy_error != OK:
			_fail_sparse_bootstrap(policy_error, "automatic_hydrolod_policy")
			return
		policy_error = subcycled.set_automatic_hydrolod_focus_provider(
			Callable(self, &"_automatic_hydrolod_focus_context"))
		if policy_error != OK:
			_fail_sparse_bootstrap(policy_error, "automatic_hydrolod_focus")
			return
	super._on_sparse_runtime_initialized(generation, runtime)


func _automatic_hydrolod_focus_context() -> Dictionary:
	if _automatic_hydrolod_focus_override_active:
		_automatic_hydrolod_focus_source = "override"
		return {
			"direction": _automatic_hydrolod_focus_override,
			"planet_radius_m": Frames.planet_radius,
		}
	var player := _local_hydrolod_player()
	if player == null:
		_automatic_hydrolod_focus_source = "none"
		return {"direction": Vector3.ZERO, "planet_radius_m": Frames.planet_radius}
	var world := Frames.to_world(player.global_position)
	var direction := Frames.world_to_dir(world)
	_automatic_hydrolod_focus_source = "local_player"
	return {
		"direction": direction,
		"planet_radius_m": Frames.planet_radius,
	}


func _local_hydrolod_player() -> Node3D:
	var tree := get_tree()
	if tree == null:
		return null
	# Explicit local-player groups take precedence when gameplay provides them.
	for group_name in [&"local_player", &"player"]:
		var explicit := tree.get_first_node_in_group(group_name)
		if explicit is Node3D:
			return explicit as Node3D

	# Asterra's character convention is the `characters` group. In multiplayer or
	# Character Studio scenes, the character nearest the active local camera is the
	# best local-control discriminator without making LOD distance camera-driven.
	var candidates: Array[Node3D] = []
	var seen: Dictionary = {}
	for node in tree.get_nodes_in_group("characters"):
		if node is Node3D:
			var candidate := node as Node3D
			seen[candidate.get_instance_id()] = true
			candidates.append(candidate)
	var scene := tree.current_scene
	if scene != null:
		_collect_named_hydrolod_characters(scene, candidates, seen)
	if candidates.is_empty():
		return null
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		return candidates[0]
	var closest: Node3D
	var closest_distance := INF
	for candidate in candidates:
		if not is_instance_valid(candidate) or candidate == camera \
				or candidate.is_ancestor_of(camera):
			continue
		var distance := camera.global_position.distance_squared_to(candidate.global_position)
		if distance < closest_distance:
			closest_distance = distance
			closest = candidate
	return closest if closest != null else candidates[0]


func _collect_named_hydrolod_characters(node: Node, result: Array[Node3D],
		seen: Dictionary) -> void:
	if node is Node3D:
		var node3d := node as Node3D
		var known := str(node3d.name) == "AsterraHuman" \
			or bool(node3d.get_meta("asterra_character", false))
		if known and not seen.has(node3d.get_instance_id()):
			seen[node3d.get_instance_id()] = true
			result.append(node3d)
	for child in node.get_children():
		_collect_named_hydrolod_characters(child, result, seen)
