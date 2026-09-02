extends "res://scripts/world_authoring/terrain_displacement_runtime_phase34.gd"
## Phase 35: native production-stage graph bridge.
##
## A canonical structural production graph is still an identity view of the
## resident GPU terrain implementation, so it emits no authored displacement
## bytecode. Stage parameters bind directly to the exact production uniforms.
## Structural rewiring is deliberately rejected here; later runtime phases may
## override the topology validation hook only for structural forms that have an
## exact resident-shader lowering. Last-known-good terrain remains active otherwise.

const NATIVE_GRAPH := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")


func _is_identity_shape_slot(slot: Resource) -> bool:
	if super._is_identity_shape_slot(slot):
		return true
	if slot == null or String(slot.get(&"slot_id")) != NATIVE_GRAPH.PRODUCTION_SHAPE_SLOT_ID:
		return false
	var graph: Resource = slot.get(&"graph") as Resource
	return NATIVE_GRAPH.is_canonical_structural_graph(graph)


func _extract_geomorph_controls(terrain: Resource) -> Dictionary:
	var out: Dictionary = super._extract_geomorph_controls(terrain)
	if terrain == null:
		return out
	var slots_value: Variant = terrain.get(&"displacement_slots")
	if not (slots_value is Array):
		return out
	for slot_value: Variant in slots_value as Array:
		var slot: Resource = slot_value as Resource
		if slot == null or not bool(slot.get(&"enabled")) or int(slot.get(&"domain")) != 0:
			continue
		var graph: Resource = slot.get(&"graph") as Resource
		if graph == null or not NATIVE_GRAPH.has_structural_nodes(graph):
			continue
		var native_controls: Dictionary = NATIVE_GRAPH.extract_controls(graph)
		for key_value: Variant in native_controls.keys():
			out[key_value] = native_controls[key_value]
	return out


func _validate_candidate(terrain: Resource) -> PackedStringArray:
	var issues: PackedStringArray = super._validate_candidate(terrain)
	if terrain == null:
		return issues
	var slots_value: Variant = terrain.get(&"displacement_slots")
	if not (slots_value is Array):
		return issues
	for slot_value: Variant in slots_value as Array:
		var slot: Resource = slot_value as Resource
		if slot == null or not bool(slot.get(&"enabled")) or int(slot.get(&"domain")) != 0:
			continue
		var graph: Resource = slot.get(&"graph") as Resource
		if graph == null or not NATIVE_GRAPH.has_structural_nodes(graph):
			continue
		if String(slot.get(&"slot_id")) != NATIVE_GRAPH.PRODUCTION_SHAPE_SLOT_ID:
			issues.append("Native production geomorph stages may only exist in Base Terrain Shape; keeping the last valid terrain.")
			continue
		var topology_issue: String = _native_topology_validation_issue(graph)
		if not topology_issue.is_empty():
			issues.append(topology_issue)
	return issues


func _native_topology_validation_issue(graph: Resource) -> String:
	if NATIVE_GRAPH.is_canonical_structural_graph(graph):
		return ""
	return "Base Terrain native-stage topology is incomplete or reordered. Exact structural lowering is not enabled yet; keeping the last valid terrain."


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["native_production_stage_bridge"] = true
	return out
