extends "res://scripts/terrain/ground_geometry_clipmap_pages_stable.gd"
## Correctness-first direct GPU-page renderer.
##
## Runtime wireframe testing proved that the six local clipmap levels are present.
## Keep them as independently culled one-instance batches for now so batching is
## not allowed to hide an entire group of rings.
##
## The coarse global terrain handoff is enabled again. With the local geometry now
## verified, leaving the global quadtree underneath causes exactly the chunk-edge
## clipping seen in wireframe: whichever coarse triangle sits above the fine page
## wins the depth test. The inherited handoff removes the global terrain only
## inside the protected inner footprint, while the outer L5 ring keeps a narrow
## dithered overlap to the global surface.

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


## The rings are now proven present. Remove the coarse global approximation under
## the inner clipmap footprint so coarse chunk boundaries cannot depth-occlude the
## fine surface. This is only called active after the stable renderer has verified
## its L6 backing coverage.
func _sync_global_cutout(enabled: bool) -> void:
	super._sync_global_cutout(enabled)


func gpu_stream_stats() -> Dictionary:
	var result: Dictionary = super.gpu_stream_stats()
	result["draw_batches"] = _level_batches.size()
	result["safe_global_backing"] = false
	return result
