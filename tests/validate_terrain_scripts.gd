extends SceneTree
## Parse/load smoke test for dependency-heavy terrain scripts.
##
## Running these through a tiny SceneTree is more representative than executing a
## Node3D script directly with --script/--check-only, which asks Godot to treat the
## target itself as the MainLoop and can fail to resolve its inheritance chain.

const SCRIPT_CHAIN := [
	"res://scripts/core/frames.gd",
	"res://scripts/terrain/gpu_terrain_scatter.gd",
	"res://scripts/terrain/gpu_terrain_scatter_compact.gd",
	"res://scripts/terrain/gpu_terrain_scatter_global.gd",
	"res://scripts/terrain/spherical_geometry_clipmap.gd",
	"res://scripts/terrain/spherical_geometry_clipmap_authoritative.gd",
]


func _init() -> void:
	var frames_script: Script
	for script_path: String in SCRIPT_CHAIN:
		var resource: Resource = load(script_path)
		if resource == null or not (resource is Script):
			push_error("TERRAIN_SCRIPT_LOAD_FAILED: %s" % script_path)
			quit(1)
			return
		var script := resource as Script
		if not script.can_instantiate():
			push_error("TERRAIN_SCRIPT_NOT_INSTANTIABLE: %s" % script_path)
			quit(1)
			return
		if script_path == "res://scripts/core/frames.gd":
			frames_script = script
		print("TERRAIN_SCRIPT_LOAD_OK: %s" % script_path)

	if not _validate_rebase_frame_barrier(frames_script):
		quit(1)
		return

	print("TERRAIN_SCRIPT_STACK_OK: %d scripts" % SCRIPT_CHAIN.size())
	quit(0)


func _validate_rebase_frame_barrier(frames_script: Script) -> bool:
	if frames_script == null:
		push_error("REBASE_FRAME_BARRIER_FAILED: Frames script was not loaded")
		return false
	var frames: Node = frames_script.new() as Node
	if frames == null:
		push_error("REBASE_FRAME_BARRIER_FAILED: Frames could not instantiate")
		return false
	frames.call("_ready")

	# Crossing the travel threshold during an arbitrary physics callback must only
	# queue the global frame mutation. Until the early Frames physics callback runs,
	# every renderer and physics consumer must still observe the old origin.
	var requested: bool = bool(frames.call("maintain_origin", Vector3(5000.0, -17.0, 31.0)))
	var before: Vec3D = frames.get("origin") as Vec3D
	if not requested or not bool(frames.call("rebase_pending")):
		push_error("REBASE_FRAME_BARRIER_FAILED: threshold crossing did not queue rebase")
		return false
	if before == null or absf(before.x) > 1e-9 or absf(before.y) > 1e-9 or absf(before.z) > 1e-9:
		push_error("REBASE_FRAME_BARRIER_FAILED: maintain_origin mutated origin mid-callback")
		return false
	if int(frames.call("rebase_count")) != 0:
		push_error("REBASE_FRAME_BARRIER_FAILED: queued request incremented rebase count early")
		return false

	# Simulate the start-of-next-physics-frame barrier. The request commits exactly
	# once, near-zeroing the requesting observer before any ordinary physics node.
	frames.call("_physics_process", 1.0 / 60.0)
	var after: Vec3D = frames.get("origin") as Vec3D
	if after == null or absf(after.x - 5000.0) > 1e-6 \
			or absf(after.y + 17.0) > 1e-6 or absf(after.z - 31.0) > 1e-6:
		push_error("REBASE_FRAME_BARRIER_FAILED: pending origin was not committed at barrier")
		return false
	if bool(frames.call("rebase_pending")) or int(frames.call("rebase_count")) != 1:
		push_error("REBASE_FRAME_BARRIER_FAILED: pending rebase did not commit exactly once")
		return false
	var rebased_render: Vector3 = frames.call("to_render", after)
	if rebased_render.length_squared() > 1e-8:
		push_error("REBASE_FRAME_BARRIER_FAILED: committed observer is not local-frame origin")
		return false

	frames.call("_physics_process", 1.0 / 60.0)
	if int(frames.call("rebase_count")) != 1:
		push_error("REBASE_FRAME_BARRIER_FAILED: empty barrier repeated prior rebase")
		return false

	print("REBASE_FRAME_BARRIER_OK: travel rebases commit once at the next early physics boundary")
	return true
