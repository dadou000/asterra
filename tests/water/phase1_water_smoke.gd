extends Node
## Runtime smoke test for the 0.1.0 Phase 1 water services.
##
## Run with a RenderingDevice renderer to exercise the shared GPU texture path.
## In --headless mode Godot intentionally exposes no global RenderingDevice, so
## the test only verifies the graceful fallback and query-service bridge.

const MAX_WAIT_FRAMES := 120
var _frames := 0


func _process(_delta: float) -> void:
	_frames += 1
	if _frames < 3:
		return

	var resources := WaterSystem.surface_resources()
	if resources != null and resources.creation_pending() and _frames < MAX_WAIT_FRAMES:
		return

	var failures: Array[String] = []
	if WaterSystem.query_service() == null:
		failures.append("HydrologyQueryService was not created")
	elif WaterSystem.query_service().backend() == null:
		failures.append("legacy OceanGPUPhysics backend was not bound")

	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		if resources == null or not resources.available():
			failures.append("main-RD dynamic water field was not allocated")
		else:
			if resources.field_texture() == null:
				failures.append("Texture2DRD wrapper was not created")
			if resources.gpu_bytes_estimate() != 256 * 256 * 4 * 4:
				failures.append("unexpected Phase 1 water field byte estimate")
			var err := WaterSystem.debug_write_surface_gaussian(1.0, 200.0,
				Vector2(0.5, 0.0), 1.0)
			if err != OK:
				failures.append("debug dynamic-water upload was not accepted: %d" % int(err))
	else:
		if resources != null and resources.available():
			failures.append("dynamic surface unexpectedly available without global RD")

	if failures.is_empty():
		print("PHASE1_WATER_SMOKE: PASS ", WaterSystem.gpu_stats())
		get_tree().quit(0)
	else:
		for failure in failures:
			push_error("PHASE1_WATER_SMOKE: " + failure)
		get_tree().quit(1)
