extends "res://scripts/world_authoring/terrain_displacement_runtime_phase35.gd"
## Phase 36: exact ordered native-stage bypass lowering.
##
## Native production stages may now be removed from the Start -> Compose flow while
## retaining their original relative order. The structural edit is lowered to exact
## controls already consumed by gpu_geomorph.gdshaderinc, so the resident production
## shader remains the sole implementation and authored displacement bytecode stays
## empty. Arbitrary reordering/branching is still rejected transactionally.

const NATIVE_LOWERING := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_lowering.gd")


func _is_identity_shape_slot(slot: Resource) -> bool:
	if super._is_identity_shape_slot(slot):
		return true
	if slot == null or String(slot.get(&"slot_id")) != NATIVE_GRAPH.PRODUCTION_SHAPE_SLOT_ID:
		return false
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null or not NATIVE_GRAPH.has_structural_nodes(graph):
		return false
	var plan: Dictionary = NATIVE_LOWERING.ordered_bypass_plan(graph)
	return bool(plan.get("valid", false))


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
		if String(slot.get(&"slot_id")) != NATIVE_GRAPH.PRODUCTION_SHAPE_SLOT_ID:
			continue
		var graph: Resource = slot.get(&"graph") as Resource
		if graph == null or not NATIVE_GRAPH.has_structural_nodes(graph):
			continue
		var plan: Dictionary = NATIVE_LOWERING.ordered_bypass_plan(graph)
		if bool(plan.get("valid", false)):
			out = NATIVE_LOWERING.apply_bypass_controls(out, plan)
		break
	return out


func _native_topology_validation_issue(graph: Resource) -> String:
	var plan: Dictionary = NATIVE_LOWERING.ordered_bypass_plan(graph)
	if bool(plan.get("valid", false)):
		return ""
	var reason: String = String(plan.get("reason", "invalid native-stage chain"))
	return "Base Terrain native-stage topology is unsupported: %s. Only production-order stage bypass is executable; keeping the last valid terrain." % reason


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["native_ordered_bypass_lowering"] = true
	return out
