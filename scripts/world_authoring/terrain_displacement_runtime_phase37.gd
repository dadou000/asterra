extends "res://scripts/world_authoring/terrain_displacement_runtime_phase36.gd"
## Phase 37-40: exact native contribution topology and typed linear composition.
##
## Saved Phase 37 linear chains remain readable. Broad through Micro may feed Native
## Detail Merge directly or form a restricted exact linear expression using Add,
## Multiply by Constant Float and Mix with a Constant Float factor. Phase 40 reduces
## that expression to resident production control coefficients; no second native
## terrain evaluator or authored displacement bytecode is introduced. Invalid or
## nonlinear topology remains candidate-only and cannot replace last-known-good.

const NATIVE_REORDER_LOWERING := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_lowering_phase40.gd")
const PHASE39_LOWERING := preload(
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

	# Phase 36 already applies every branch form understood by the green Phase 39
	# lowering. Do not apply those controls again. If Phase 39 cannot understand a
	# valid Phase 40 branch, the new linear reducer is the sole control publisher.
	if bool(plan.get("branching", false)):
		var phase39_plan: Dictionary = _extract_phase39_plan(terrain)
		if not bool(phase39_plan.get("valid", false)):
			out = NATIVE_REORDER_LOWERING.apply_bypass_controls(out, plan)
		return out

	# Legacy reordered Phase 37 chains are deliberately rejected by Phase 36 and are
	# therefore still normalized exactly once here.
	out = NATIVE_REORDER_LOWERING.apply_bypass_controls(out, plan)
	return out


func _native_topology_validation_issue(graph: Resource) -> String:
	var plan: Dictionary = NATIVE_REORDER_LOWERING.commutative_reorder_plan(graph)
	if bool(plan.get("valid", false)):
		return ""
	var reason: String = String(plan.get("reason", "invalid native terrain graph"))
	return "Base Terrain native topology is unsupported: %s. Safe native math is Add Contributions, Scale Contribution (Multiply by Constant Float 0..4), or Blend Contributions (Mix with Constant Float 0..1). Multi-stage Add/Mix results must use a Custom Group Merge socket. Dynamic masks, negative coefficients and nonlinear terrain multiplication are not enabled. Keeping the last valid terrain." % reason


func _extract_reorder_plan(terrain: Resource) -> Dictionary:
	var graph: Resource = _find_native_graph(terrain)
	if graph == null:
		return {}
	return NATIVE_REORDER_LOWERING.commutative_reorder_plan(graph)


func _extract_phase39_plan(terrain: Resource) -> Dictionary:
	var graph: Resource = _find_native_graph(terrain)
	if graph == null:
		return {}
	return PHASE39_LOWERING.commutative_reorder_plan(graph)


func _find_native_graph(terrain: Resource) -> Resource:
	if terrain == null:
		return null
	var slots_value: Variant = terrain.get(&"displacement_slots")
	if not (slots_value is Array):
		return null
	for slot_value: Variant in slots_value as Array:
		var slot: Resource = slot_value as Resource
		if slot == null or not bool(slot.get(&"enabled")) or int(slot.get(&"domain")) != 0:
			continue
		if String(slot.get(&"slot_id")) != NATIVE_GRAPH.PRODUCTION_SHAPE_SLOT_ID:
			continue
		var graph: Resource = slot.get(&"graph") as Resource
		if graph != null and NATIVE_GRAPH.has_structural_nodes(graph):
			return graph
	return null


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["native_commutative_reorder_lowering"] = true
	out["native_contribution_merge_lowering"] = true
	out["native_typed_scale_lowering"] = true
	out["native_typed_add_lowering"] = true
	out["native_typed_mix_lowering"] = true
	out["native_linear_expression_lowering"] = true
	out["native_reorder_plan"] = _last_native_reorder_plan.duplicate(true)
	return out
