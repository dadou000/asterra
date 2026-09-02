extends "res://scripts/world_authoring/validate_terrain_program_transaction_ci.gd"
## Phase 29 regression: staged graph edits are runtime inputs immediately.
##
## No session Apply occurs in this test. A node parameter mutation increments the
## graph revision, which must change the terrain-profile fingerprint and produce a
## new compact bytecode generation. The production clipmap polls this fingerprint
## once per rendered frame, naturally coalescing all edits that occur before that
## frame into one compile/rebind.

const LIVE_TERRAIN_PROFILE := preload(
	"res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const LIVE_SLOT_MODEL := preload(
	"res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const LIVE_DISPLACEMENT_RUNTIME := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime.gd")
const LIVE_MATERIAL_RUNTIME := preload(
	"res://scripts/world_authoring/terrain_material_runtime.gd")


func _ready() -> void:
	if not _validate_live_graph_recompile_contract():
		return
	super._ready()


func _validate_live_graph_recompile_contract() -> bool:
	var terrain: Resource = LIVE_TERRAIN_PROFILE.new()
	terrain.call("ensure_valid")

	# Displacement: prove a staged numeric parameter changes both the fingerprint
	# and the authoritative CPU/GPU bytecode result without calling Apply.
	var displacement_slot: Resource = terrain.call("create_shader_slot",
		LIVE_SLOT_MODEL.Domain.DISPLACEMENT, "CI Realtime Displacement") as Resource
	var displacement_graph: Resource = displacement_slot.get(&"graph") as Resource \
		if displacement_slot != null else null
	if displacement_graph == null:
		_fail("LIVE_GRAPH_RECOMPILE_FAILED: displacement graph was not created")
		return false
	var displacement_output: String = _live_find_output(
		displacement_graph, "OUTPUT_DISPLACEMENT")
	var height_node: String = String(displacement_graph.call("add_node",
		"CONSTANT_FLOAT", Vector2(120.0, 120.0), {"value": 12.5}))
	if displacement_output.is_empty() or not bool(displacement_graph.call(
			"connect_nodes", height_node, 0, displacement_output, 0)):
		_fail("LIVE_GRAPH_RECOMPILE_FAILED: displacement test graph did not connect")
		return false

	var displacement_runtime: Node = LIVE_DISPLACEMENT_RUNTIME.new() as Node
	var displacement_fingerprint_a: String = String(displacement_runtime.call(
		"profile_fingerprint", terrain))
	var displacement_stats_a: Dictionary = displacement_runtime.call(
		"compile_from_terrain", terrain) as Dictionary
	var height_a: float = float(displacement_runtime.call("evaluate_height",
		Vector3.RIGHT, 0.0, 0, 0, 0.0))

	displacement_graph.call("set_node_parameter", height_node, "value", 64.0)
	var displacement_fingerprint_b: String = String(displacement_runtime.call(
		"profile_fingerprint", terrain))
	if displacement_fingerprint_b == displacement_fingerprint_a:
		displacement_runtime.free()
		_fail("LIVE_GRAPH_RECOMPILE_FAILED: displacement parameter edit did not invalidate fingerprint")
		return false
	var displacement_stats_b: Dictionary = displacement_runtime.call(
		"compile_from_terrain", terrain) as Dictionary
	var height_b: float = float(displacement_runtime.call("evaluate_height",
		Vector3.RIGHT, 0.0, 0, 0, 0.0))
	displacement_runtime.free()
	if int(displacement_stats_b.get("generation", 0)) \
			!= int(displacement_stats_a.get("generation", 0)) + 1:
		_fail("LIVE_GRAPH_RECOMPILE_FAILED: displacement bytecode generation did not advance")
		return false
	if not is_equal_approx(height_a, 12.5) or not is_equal_approx(height_b, 64.0):
		_fail("LIVE_GRAPH_RECOMPILE_FAILED: staged displacement parameter did not change authoritative result")
		return false

	# Material: the fragment program has no CPU shading evaluator, so validate its
	# exact runtime invalidation contract: revision -> fingerprint -> new bytecode.
	var material_slot: Resource = terrain.call("create_shader_slot",
		LIVE_SLOT_MODEL.Domain.MATERIAL, "CI Realtime Material") as Resource
	var material_graph: Resource = material_slot.get(&"graph") as Resource \
		if material_slot != null else null
	if material_graph == null:
		_fail("LIVE_GRAPH_RECOMPILE_FAILED: material graph was not created")
		return false
	var material_output: String = _live_find_output(material_graph, "OUTPUT_MATERIAL")
	var color_node: String = String(material_graph.call("add_node",
		"CONSTANT_COLOR", Vector2(120.0, 120.0), {"value": Color(0.15, 0.25, 0.35, 1.0)}))
	if material_output.is_empty() or not bool(material_graph.call(
			"connect_nodes", color_node, 0, material_output, 0)):
		_fail("LIVE_GRAPH_RECOMPILE_FAILED: material test graph did not connect")
		return false

	var material_runtime: Node = LIVE_MATERIAL_RUNTIME.new() as Node
	var material_fingerprint_a: String = String(material_runtime.call(
		"profile_fingerprint", terrain))
	var material_stats_a: Dictionary = material_runtime.call(
		"compile_from_terrain", terrain) as Dictionary
	material_graph.call("set_node_parameter", color_node, "value",
		Color(0.75, 0.45, 0.20, 1.0))
	var material_fingerprint_b: String = String(material_runtime.call(
		"profile_fingerprint", terrain))
	if material_fingerprint_b == material_fingerprint_a:
		material_runtime.free()
		_fail("LIVE_GRAPH_RECOMPILE_FAILED: material parameter edit did not invalidate fingerprint")
		return false
	var material_stats_b: Dictionary = material_runtime.call(
		"compile_from_terrain", terrain) as Dictionary
	material_runtime.free()
	if not bool(material_stats_a.get("active", false)) \
			or not bool(material_stats_b.get("active", false)):
		_fail("LIVE_GRAPH_RECOMPILE_FAILED: material bytecode program is inactive")
		return false
	if int(material_stats_b.get("generation", 0)) \
			!= int(material_stats_a.get("generation", 0)) + 1:
		_fail("LIVE_GRAPH_RECOMPILE_FAILED: material bytecode generation did not advance")
		return false

	print("LIVE_GRAPH_RECOMPILE_OK: staged displacement/material parameters invalidate and recompile without Apply")
	return true


func _live_find_output(graph: Resource, node_type: String) -> String:
	for node_value: Variant in graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		if String(node.get("type", "")) == node_type:
			return String(node.get("id", ""))
	return ""
