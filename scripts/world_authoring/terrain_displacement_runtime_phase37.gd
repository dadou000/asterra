extends "res://scripts/world_authoring/terrain_displacement_runtime_phase36.gd"
## Phase 37-39: exact native contribution topology and typed scale composition.
##
## Saved Phase 37 linear chains remain readable. Broad through Micro may also feed
## Native Detail Merge directly, or pass through the constrained typed pattern
## Native -> Multiply <- Constant Float -> matching Merge input. That pattern lowers
## into existing resident production controls, so it still emits zero authored
## displacement bytecode. Invalid topology remains candidate-only and cannot replace
## the last-known-good terrain snapshot.

const NATIVE_REORDER_LOWERING := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_lowering.gd")

var _last_native_reorder_plan: Dictionary = {}


func clear() -> void:
	super.clear()
	_last_native_reorder_plan = {}


func compile_from_terrain(terrain: Resource) -> Dictionary:
	var result: Dictionary = super.compile_from_terrain(terrain)
	if bool(result.get("candidate_valid", true)):
		_last_native_reorder_plan = _extract_reorder_plan(terrain)
	result["native_reorder_plan"] = _last_native_reorder_plan.duplicate(true)
	return result


func _is_identity_shape_slot(slot: Resource) -> bool:
	if super._is_identity_shape_slot(slot):
		return true
	if slot == null or String(slot.get(&"slot_id")) != NATIVE_GRAPH.PRODUCTION_SHAPE_SLOT_ID:
		return false
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null or not NATIVE_GRAPH.has_structural_nodes(graph):
		return false
	var plan: Dictionary = NATIVE_REORDER_LOWERING.commutative_reorder_plan(graph)
	return bool(plan.get("valid", false))


func _extract_geomorph_controls(terrain: Resource) -> Dictionary:
	var out: Dictionary = super._extract_geomorph_controls(terrain)
	var plan: Dictionary = _extract_reorder_plan(terrain)
	if not bool(plan.get("valid", false)):
		return out
	# Phase 36 already recognizes every merge-based branch graph through
	# ordered_bypass_plan(), which delegates to contribution_merge_plan(). Applying
	# the same plan again was harmless for zero-valued bypass controls but would square
	# scale factors (0.5 -> 0.25). Only legacy reordered linear graphs need the Phase
	# 37 pass because Phase 36 intentionally rejects their reordered serialization.
	if not bool(plan.get("branching", false)):
		out = NATIVE_REORDER_LOWERING.apply_bypass_controls(out, plan)
	return out


func _native_topology_validation_issue(graph: Resource) -> String:
	var plan: Dictionary = NATIVE_REORDER_LOWERING.commutative_reorder_plan(graph)
	if bool(plan.get("valid", false)):
		return ""
	var reason: String = String(plan.get("reason", "invalid native terrain graph"))
	return "Base Terrain native topology is unsupported: %s. Contributions must return to their matching Native Detail Merge sockets. Safe scaling is Native contribution -> Multiply <- Constant Float (0..4) -> matching Merge; Glacial may follow Merge or be bypassed. Keeping the last valid terrain." % reason


func _extract_reorder_plan(terrain: Resource) -> Dictionary:
	if terrain == null:
		return {}
	var slots_value: Variant = terrain.get(&"displacement_slots")
	if not (slots_value is Array):
		return {}
	for slot_value: Variant in slots_value as Array:
		var slot: Resource = slot_value as Resource
		if slot == null or not bool(slot.get(&"enabled")) or int(slot.get(&"domain")) != 0:
			continue
		if String(slot.get(&"slot_id")) != NATIVE_GRAPH.PRODUCTION_SHAPE_SLOT_ID:
			continue
		var graph: Resource = slot.get(&"graph") as Resource
		if graph == null or not NATIVE_GRAPH.has_structural_nodes(graph):
			continue
		return NATIVE_REORDER_LOWERING.commutative_reorder_plan(graph)
	return {}


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["native_commutative_reorder_lowering"] = true
	out["native_contribution_merge_lowering"] = true
	out["native_typed_scale_lowering"] = true
	out["native_reorder_plan"] = _last_native_reorder_plan.duplicate(true)
	return out
