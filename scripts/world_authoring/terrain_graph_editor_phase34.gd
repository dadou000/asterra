extends "res://scripts/world_authoring/terrain_graph_editor_phase33.gd"
## Phase 34: non-destructive production-control migration.
##
## Older saved production graphs can predate newly exposed settings nodes. When a
## graph still consumes the production generated-height or PBR boundary, insert
## only missing unconnected settings nodes. Custom data-flow links are untouched.

const SURFACE_STAGE_TYPES: Array[String] = [
	"PRODUCTION_ALBEDO",
	"PRODUCTION_NORMAL",
	"PRODUCTION_ROUGHNESS",
	"PRODUCTION_METALLIC",
	"PRODUCTION_AO",
	"PRODUCTION_SPECULAR",
]

const CONTROL_POSITIONS: Dictionary = {
	"PRODUCTION_GEOMORPH_SETTINGS":Vector2(70.0, 300.0),
	"PRODUCTION_CLASSIFIER_SETTINGS":Vector2(1080.0, 30.0),
	"PRODUCTION_CLASSIFIER_THRESHOLDS":Vector2(1080.0, 500.0),
	"PRODUCTION_SURFACE_PALETTE":Vector2(1080.0, 1050.0),
	"PRODUCTION_MICRORELIEF_SETTINGS":Vector2(1440.0, 30.0),
	"PRODUCTION_ANTITILE_SETTINGS":Vector2(1440.0, 370.0),
	"PRODUCTION_ROCK_PBR_SETTINGS":Vector2(1800.0, 30.0),
	"PRODUCTION_SCAN_PBR_SETTINGS":Vector2(2160.0, 30.0),
	"PRODUCTION_SCAN_TEXTURES":Vector2(2520.0, 30.0),
}


func setup(session: RefCounted, slot: Resource,
		rebuild_requested: Callable = Callable()) -> void:
	_migrate_missing_production_controls(slot)
	super.setup(session, slot, rebuild_requested)


func _migrate_missing_production_controls(slot: Resource) -> void:
	if slot == null:
		return
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return
	var nodes_value: Variant = graph.get(&"nodes")
	if not (nodes_value is Array):
		return
	var domain: int = int(graph.get(&"domain"))
	var existing: Dictionary = {}
	var has_generated_stage: bool = false
	var has_surface_stage: bool = false
	for node_value: Variant in nodes_value as Array:
		if not (node_value is Dictionary):
			continue
		var node_type: String = String((node_value as Dictionary).get("type", ""))
		existing[node_type] = true
		has_generated_stage = has_generated_stage or node_type == "PRODUCTION_GENERATED_HEIGHT"
		has_surface_stage = has_surface_stage or SURFACE_STAGE_TYPES.has(node_type)

	if domain == GRAPH_SCRIPT.Domain.DISPLACEMENT:
		if has_generated_stage and not existing.has("PRODUCTION_GEOMORPH_SETTINGS"):
			_add_missing_control(graph, "PRODUCTION_GEOMORPH_SETTINGS")
		return
	if domain != GRAPH_SCRIPT.Domain.MATERIAL or not has_surface_stage:
		return
	for control_type: String in GRAPH_SCRIPT.PRODUCTION_CONTROL_NODES:
		if control_type == "PRODUCTION_GEOMORPH_SETTINGS" or existing.has(control_type):
			continue
		_add_missing_control(graph, control_type)


func _add_missing_control(graph: Resource, node_type: String) -> void:
	var position: Vector2 = CONTROL_POSITIONS.get(node_type, Vector2(1200.0, 100.0)) as Vector2
	graph.call("add_node", node_type, position,
		GRAPH_SCRIPT.production_control_defaults(node_type))
