extends "res://scripts/terrain/spherical_geometry_clipmap_cache_contract_phase42.gd"
## Phase 29 terrain runtime bridge.
##
## Keep the mature Blank/Procedural clipmap ownership while installing the Phase 29
## displacement compiler whose production graphs use absolute-height semantics.
## Phase 42A inserts only the cache-factory contract below this runtime chain; all
## topology, handoff, Blank backend and authoring behavior remain inherited.

const PHASE29_DISPLACEMENT_RUNTIME := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase29.gd")

# The dense 0.25 m micro lattice only changes the rendered surface while the
# microrelief shader is inside its 90-160 m camera-distance envelope. Keeping the
# ~100k-triangle disc alive above that envelope wastes vertex work while producing
# the same coarse surface. Hysteresis prevents topology toggling around the cutoff.
const MICRO_TOPOLOGY_ENABLE_AGL_M: float = 140.0
const MICRO_TOPOLOGY_DISABLE_AGL_M: float = 180.0
var _micro_topology_distance_enabled := true


func _process(dt: float) -> void:
	super._process(dt)
	# Active LOD can remain at L0 while altitude changes inside the surface guard,
	# so refresh the micro topology gate even when the logical LOD window is stable.
	_sync_micro_lod_state()


func _sync_micro_lod_state() -> void:
	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera != null and _planet().cfg != null:
		var observer_world_d: Vec3D = _obs_world(camera)
		var observer_world: Vector3 = observer_world_d.to_v3()
		var observer_radius: float = observer_world.length()
		if observer_radius > 1.0:
			var observer_dir: Vector3 = observer_world / observer_radius
			var macro_h: float = _planet().macro_height(observer_dir)
			var agl_m: float = observer_radius - (_planet().cfg.planet_radius + macro_h)
			if _micro_topology_distance_enabled:
				if agl_m > MICRO_TOPOLOGY_DISABLE_AGL_M:
					_micro_topology_distance_enabled = false
			else:
				if agl_m < MICRO_TOPOLOGY_ENABLE_AGL_M:
					_micro_topology_distance_enabled = true

	var want_micro: bool = _active_min_level == 0 \
		and debug_level_enabled(0) \
		and _micro_topology_distance_enabled
	if want_micro != _micro_l0_active:
		_micro_l0_active = want_micro
		_apply_center_mesh_variant()
	if _micro_batch != null:
		_micro_batch.visible = _micro_should_be_visible()


func _ensure_displacement_runtime() -> void:
	if _displacement_runtime != null and is_instance_valid(_displacement_runtime):
		# Replace an older runtime if this script was hot-reloaded into an existing
		# editor session.
		if _displacement_runtime.get_script() == PHASE29_DISPLACEMENT_RUNTIME:
			return
		_displacement_runtime.remove_from_group(&"terrain_displacement_runtime")
		_displacement_runtime.queue_free()
		_displacement_runtime = null
	_displacement_runtime = PHASE29_DISPLACEMENT_RUNTIME.new() as Node
	if _displacement_runtime == null:
		return
	_displacement_runtime.name = "TerrainDisplacementRuntime"
	add_child(_displacement_runtime)
	_displacement_fingerprint = ""
