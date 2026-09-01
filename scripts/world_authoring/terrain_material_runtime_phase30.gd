extends "res://scripts/world_authoring/terrain_material_runtime.gd"
## Phase 30 keeps the visible Base Surface graph free when it is still an exact
## production pass-through. The serialized graph remains fully editable in the UI;
## bytecode becomes active as soon as its topology/source selection changes.

const PRODUCTION_SURFACE_SLOT_ID := "production-terrain-surface"
const REQUIRED_BASE_SOURCES: Array[String] = [
	"base_albedo", "base_normal", "base_roughness",
	"base_metallic", "base_ao", "base_specular",
]


func compile_from_terrain(terrain: Resource) -> Dictionary:
	if terrain == null:
		return super.compile_from_terrain(terrain)
	var skipped: Array[Resource] = []
	var slots_value: Variant = terrain.get(&"material_slots")
	if slots_value is Array:
		for slot_value: Variant in slots_value as Array:
			var slot: Resource = slot_value as Resource
			if slot != null and bool(slot.get(&"enabled")) and _is_identity_surface_slot(slot):
				slot.set(&"enabled", false)
				skipped.append(slot)
	var stats: Dictionary = super.compile_from_terrain(terrain)
	for slot: Resource in skipped:
		slot.set(&"enabled", true)
	_fingerprint = profile_fingerprint(terrain)
	return stats


func _is_identity_surface_slot(slot: Resource) -> bool:
	if String(slot.get(&"slot_id")) != PRODUCTION_SURFACE_SLOT_ID:
		return false
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return false
	var nodes: Array = graph.get(&"nodes") as Array
	var links: Array = graph.get(&"links") as Array
	if nodes.size() != 7 or links.size() != 6:
		return false
	var output_id: String = ""
	var source_by_id: Dictionary = {}
	for node_value: Variant in nodes:
		var node: Dictionary = node_value as Dictionary
		var node_type: String = String(node.get("type", ""))
		var node_id: String = String(node.get("id", ""))
		if node_type == "OUTPUT_MATERIAL":
			output_id = node_id
		elif node_type == "GAME_INPUT":
			var parameters: Dictionary = node.get("parameters", {}) as Dictionary
			source_by_id[node_id] = String(parameters.get("source", ""))
		else:
			return false
	if output_id.is_empty() or source_by_id.size() != 6:
		return false
	var seen: Dictionary = {}
	for link_value: Variant in links:
		var link: Dictionary = link_value as Dictionary
		if String(link.get("to", "")) != output_id:
			return false
		var port: int = int(link.get("to_port", -1))
		if port < 0 or port >= REQUIRED_BASE_SOURCES.size():
			return false
		var source: String = String(source_by_id.get(String(link.get("from", "")), ""))
		if source != REQUIRED_BASE_SOURCES[port]:
			return false
		seen[port] = true
	return seen.size() == REQUIRED_BASE_SOURCES.size()
