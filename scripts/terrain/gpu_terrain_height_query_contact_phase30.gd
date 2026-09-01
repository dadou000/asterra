extends "res://scripts/terrain/gpu_terrain_height_query_contact.gd"
## Phase 30 contact parity for the production Shape graph.
##
## The visual bytecode receives the existing sparse sculpt/edit delta as an explicit
## graph input. Feed the same value to the CPU evaluator. The inherited contact path
## still adds Deltas afterward; the absolute compiler subtracts the same baseline,
## so an unchanged Base Terrain graph is neutral and sculpt edits stay authoritative.


func _authored_displacement(direction: Vector3, base_height_m: float,
		biome_id: int) -> float:
	if _author_runtime == null or not is_instance_valid(_author_runtime):
		_author_runtime = get_tree().get_first_node_in_group(&"terrain_displacement_runtime")
	if _author_runtime == null or not _author_runtime.has_method("evaluate_height"):
		return 0.0
	var sculpt_delta_m: float = 0.0 if _is_blank_backend() else Deltas.offset_at(direction)
	return float(_author_runtime.call("evaluate_height", direction, base_height_m,
		AUTHOR_CONTACT_LEVEL, biome_id, NAN, sculpt_delta_m))
