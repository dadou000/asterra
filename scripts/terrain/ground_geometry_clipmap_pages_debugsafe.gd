extends "res://scripts/terrain/ground_geometry_clipmap_pages_stable.gd"
## Correctness-first direct GPU-page renderer.
##
## The previous renderer batched L1-L5 into one MultiMesh submission. Runtime
## wireframe captures showed the global terrain cutout but no corresponding ring
## geometry, so this path deliberately removes multi-instance batching as a
## variable: each clipmap level owns a one-instance MultiMesh with its own custom
## data and culling object.
##
## The global terrain cutout is disabled in this diagnostic-safe path. Until the
## local renderer is proven complete, the coarse planet is a safety backing layer
## rather than a square hole. Once the six rings are verified we can reintroduce
## the handoff using a validity mask/sink instead of an unconditional discard.

var _level_batches: Array[MultiMeshInstance3D] = []


func _build_ring_nodes() -> void:
	_level_batches.clear()
	var full: ArrayMesh = _build_strip_mesh(false)
	var ring: ArrayMesh = _build_strip_mesh(true)
	var bounds := AABB(
		Vector3(-100000.0, -100000.0, -100000.0),
		Vector3(200000.0, 200000.0, 200000.0))

	for level: int in RENDER_LEVELS:
		var mesh: ArrayMesh = full if level == 0 else ring
		var batch: MultiMeshInstance3D = _make_single_level_batch(
			"GroundClipmapL%d" % level, mesh, level,
			level == RENDER_LEVELS - 1, bounds)
		_level_batches.append(batch)
		add_child(batch)


func _make_single_level_batch(node_name: String, mesh: ArrayMesh, level: int,
		outer: bool, bounds: AABB) -> MultiMeshInstance3D:
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_custom_data = true
	mm.mesh = mesh
	mm.custom_aabb = bounds
	mm.instance_count = 1
	mm.visible_instance_count = 1
	mm.set_instance_transform(0, Transform3D.IDENTITY)
	mm.set_instance_custom_data(0, Color(
		float(level), 1.0 if outer else 0.0, 0.0, 0.0))

	var batch := MultiMeshInstance3D.new()
	batch.name = node_name
	batch.multimesh = mm
	batch.material_override = _material
	return batch


func _set_visible(value: bool) -> void:
	for batch: MultiMeshInstance3D in _level_batches:
		batch.visible = value


## Never remove the global safety terrain while diagnosing local-page coverage.
## This guarantees that a missing/invalid local page cannot become a black void.
func _sync_global_cutout(_enabled: bool) -> void:
	super._sync_global_cutout(false)


func gpu_stream_stats() -> Dictionary:
	var result: Dictionary = super.gpu_stream_stats()
	result["draw_batches"] = _level_batches.size()
	result["safe_global_backing"] = true
	return result
