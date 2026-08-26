extends "res://scripts/terrain/spherical_geometry_clipmap_global.gd"
## GPU synthesis extension for the current 0.0.5 resident global clipmap.
##
## This class deliberately inherits the latest global renderer instead of
## replacing its topology/LOD implementation. Therefore the following remain
## authoritative and unchanged:
##   - compact UV-addressed centre/ring sectors,
##   - screen-space promotion of the centre disc through L0..L14,
##   - per-level stationary lattice snapping,
##   - double-precision stable anchor reconstruction,
##   - resident Planet.global_height_texture macro terrain,
##   - resident Planet.global_material_texture fallback,
##   - dynamic sector visibility.
##
## The only draw-window change here is that active annuli are physically packed
## into each sector MultiMesh when the LOD window changes. We no longer leave a
## 14-instance allocation alive and rely only on visible_instance_count. This
## makes the GPU buffer agree with the logical LOD window and lets horizon-culling
## completely remove levels which cannot contribute from the current altitude.

const GPU_GEOMORPH_SHADER_PATH := "res://shaders/spherical_geometry_clipmap_global_gpu.gdshader"
const HORIZON_RING_SAFETY: float = 1.015

var _gpu_ctx_generation: int = -1
var _physical_ring_count: int = 0
var _horizon_max_level: int = 0


func _ready() -> void:
	# Let the complete latest 0.0.5 clipmap initialize its topology, stable anchor
	# state and resident global textures first.
	super._ready()
	_material.shader = load(GPU_GEOMORPH_SHADER_PATH)
	# Shader replacement clears its parameter bindings, so explicitly restore the
	# latest global height/material resources before layering context on top.
	super._bind_gpu_resources(true)
	super._sync_material_control()
	_bind_gpu_context(true)


func _process(dt: float) -> void:
	# Movement, promoted-centre selection and stationary lattice logic remain in
	# the latest parent implementation.
	super._process(dt)
	_bind_gpu_context(false)


func _update_active_levels() -> void:
	_update_screen_space_min_level()

	# A coarse annulus can contribute only if its inner edge intersects the
	# horizon-safe visible cap. Stop at the first level whose entire annulus starts
	# beyond that cap. This is stronger than drawing a broad tail and discarding it
	# later in the fragment shader.
	var max_level: int = _active_min_level
	var visible_radius_m: float = maxf(
		_visible_cap_arc_m,
		_base_spacing * pow(2.0, float(_active_min_level)) * float(HALF_CELLS))
	while max_level < MAX_LEVEL:
		var candidate: int = max_level + 1
		var candidate_spacing: float = _base_spacing * pow(2.0, float(candidate))
		var candidate_inner_m: float = candidate_spacing * float(RING_INNER_HALF_CELLS)
		if candidate_inner_m > visible_radius_m * HORIZON_RING_SAFETY:
			break
		max_level = candidate

	_active_max_level = maxi(max_level, _active_min_level)
	_horizon_max_level = _active_max_level
	_apply_active_level_window()


func _apply_active_level_window() -> void:
	if _active_min_level == _last_applied_min_level \
			and _active_max_level == _last_applied_max_level:
		return
	_last_applied_min_level = _active_min_level
	_last_applied_max_level = _active_max_level
	var ring_count: int = maxi(_active_max_level - _active_min_level, 0)
	_physical_ring_count = ring_count

	for sector: int in SECTOR_COUNT:
		var center: MultiMeshInstance3D = _center_sector_batches[sector]
		if center.multimesh != null:
			center.multimesh.set_instance_custom_data(0,
				Color(float(_active_min_level), float(sector), 0.0, 0.0))

		var rings: MultiMeshInstance3D = _sector_batches[sector]
		if rings.multimesh == null:
			continue
		var mm: MultiMesh = rings.multimesh

		# Resizing clears the per-instance buffers. This happens only when the
		# screen-space/horizon LOD window changes, never during ordinary movement.
		# It guarantees that the GPU contains exactly the active annuli rather than
		# a permanently allocated L1..L14 tail with a mutable draw count.
		if mm.instance_count != ring_count:
			mm.visible_instance_count = 0
			mm.instance_count = ring_count

		for instance_index: int in ring_count:
			var logical_level: int = _active_min_level + instance_index + 1
			mm.set_instance_transform(instance_index, Transform3D.IDENTITY)
			mm.set_instance_custom_data(instance_index,
				Color(float(logical_level), float(sector), 1.0, 0.0))
		mm.visible_instance_count = ring_count


func _restore_dynamic_ring_window() -> void:
	# Visibility/debug operations must never resurrect a horizon-culled level.
	var ring_count: int = _physical_ring_count
	for batch: MultiMeshInstance3D in _sector_batches:
		if batch.multimesh != null:
			batch.multimesh.visible_instance_count = mini(
				ring_count, batch.multimesh.instance_count)


func _bind_gpu_context(force: bool) -> void:
	if _material == null:
		return
	var context: Node = get_node_or_null("/root/PlanetContext")
	if context == null or not bool(context.get("ready_state")):
		_material.set_shader_parameter("u_ctx_ready", 0.0)
		return

	var generation: int = int(context.get("generation"))
	if force or generation != _gpu_ctx_generation:
		_gpu_ctx_generation = generation
		_material.set_shader_parameter("u_ctx_soil", context.get("soil_texture"))
		_material.set_shader_parameter("u_ctx_surface", context.get("surface_texture"))
		_material.set_shader_parameter("u_ctx_geology", context.get("geology_texture"))
		_material.set_shader_parameter("u_ctx_structure", context.get("structure_texture"))
		_material.set_shader_parameter("u_ctx_climate", context.get("climate_texture"))
		_material.set_shader_parameter("u_ctx_hydrology", context.get("hydrology_texture"))
		_material.set_shader_parameter("u_ctx_rock", context.get("rock_texture"))
		_material.set_shader_parameter("u_ctx_biome", context.get("biome_texture"))
		_material.set_shader_parameter("u_ctx_face_res", float(context.get("face_res")))
	_material.set_shader_parameter("u_ctx_ready", 1.0)


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	var context: Node = get_node_or_null("/root/PlanetContext")
	out["gpu_geomorph_latest_clipmap_base"] = true
	out["gpu_context_ready"] = context != null and bool(context.get("ready_state"))
	out["gpu_context_generation"] = _gpu_ctx_generation
	out["physical_ring_instances"] = _physical_ring_count
	out["horizon_max_level"] = _horizon_max_level
	out["horizon_exact_ring_buffers"] = true
	return out
