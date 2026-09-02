extends "res://scripts/terrain/gpu_terrain_clipmap_cache_phase42_active.gd"
## Final Phase 42A cache activation.
##
## A production-control edit can occur while the render thread is still creating
## the storage buffer. The initialization payload is necessarily a snapshot taken
## earlier, so always upload the latest CPU-side bytes once the RID becomes visible
## on the main thread. Subsequent changes continue through set_production_controls().


func _on_initialized_phase42(success: bool, shader: RID, pipeline: RID,
		sampler: RID, cache: RID, controls: RID) -> void:
	super._on_initialized_phase42(success, shader, pipeline, sampler, cache, controls)
	if success and controls.is_valid():
		RenderingServer.call_on_render_thread(_render_update_geomorph_controls.bind(
			controls, _geomorph_control_bytes.duplicate()))
