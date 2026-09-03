extends "res://scripts/world_authoring/terrain_displacement_runtime.gd"
## Transactional safety layer for live-authored terrain displacement programs.
##
## The base runtime is intentionally a compact compiler/VM, but it predates an
## interactive graph editor and therefore compiles directly into its live state.
## That is unsafe while a user is dragging wires: a disconnected output, cycle,
## unsupported node or instruction overflow can briefly replace a known-good
## planet with a zero/partial program. This layer turns every compile into a
## candidate transaction. Invalid candidates are rejected and the previous active
## bytecode/texture remains bound until a complete valid candidate is available.
##
## `_fingerprint` tracks the last attempted profile so a permanently invalid edit
## is not recompiled every frame. `_active_fingerprint` separately records which
## profile actually owns the currently active bytecode.

const REQUIRED_INPUTS_BY_NODE: Dictionary = {
	"ABS": 1,
	"SATURATE": 1,
	"ONE_MINUS": 1,
	"NOISE_LAYER": 1,
	"RIDGED_MOUNTAINS": 1,
	"EROSION_CHANNELS": 1,
	"SEDIMENT_DEPOSIT": 1,
	"BILLOW_NOISE": 1,
	"VORONOI_RIDGES": 1,
	"ADD": 2,
	"SUBTRACT": 2,
	"MULTIPLY": 2,
	"DIVIDE": 2,
	"MIN": 2,
	"MAX": 2,
	"POWER": 2,
	"CLAMP": 3,
	"SMOOTHSTEP": 3,
	"REMAP": 3,
	"MIX": 3,
}

var _active_fingerprint: String = ""
var _last_attempt_fingerprint: String = ""
var _last_candidate_valid: bool = true
var _last_candidate_warnings := PackedStringArray()
var _attempt_generation: int = 0
var _rejected_candidates: int = 0


func clear() -> void:
	super.clear()
	_active_fingerprint = ""
	_last_attempt_fingerprint = ""
	_last_candidate_valid = true
	_last_candidate_warnings = PackedStringArray()
	_attempt_generation = 0
	_rejected_candidates = 0


func compile_from_terrain(terrain: Resource) -> Dictionary:
	_attempt_generation += 1
	var candidate_fingerprint: String = profile_fingerprint(terrain)
	_last_attempt_fingerprint = candidate_fingerprint

	var validation_warnings: PackedStringArray = _validate_candidate(terrain)
	if not validation_warnings.is_empty():
		_last_candidate_valid = false
		_last_candidate_warnings = validation_warnings
		_rejected_candidates += 1
		# Remember the attempted document without changing the active program. This
		# prevents frame polling from repeatedly compiling the same broken edit.
		_fingerprint = candidate_fingerprint
		return _candidate_stats()

	var previous: Dictionary = _capture_active_state()
	var candidate_stats: Dictionary = super.compile_from_terrain(terrain)
	var compiler_warnings: PackedStringArray = _warnings.duplicate()

	# The legacy compiler deliberately fails soft by substituting zero for several
	# unsupported/error cases. In a live terrain editor those warnings are fatal:
	# accepting the partial program can remove large parts of the terrain.
	if not compiler_warnings.is_empty():
		_restore_active_state(previous)
		_last_candidate_valid = false
		_last_candidate_warnings = compiler_warnings
		_rejected_candidates += 1
		_fingerprint = candidate_fingerprint
		return _candidate_stats()

	_last_candidate_valid = true
	_last_candidate_warnings = PackedStringArray()
	_active_fingerprint = candidate_fingerprint
	_last_attempt_fingerprint = candidate_fingerprint
	# super already committed the complete candidate synchronously. No material can
	# observe an intermediate state because binding happens after this call returns.
	return _merge_candidate_metadata(candidate_stats)


func retag_current_profile(fingerprint: String) -> void:
	# Some compatibility layers compile an intentionally transformed duplicate of
	# the authoring resource. Retag only after that transaction completes so polling
	# compares against the real source document rather than the temporary view.
	_fingerprint = fingerprint
	_last_attempt_fingerprint = fingerprint
	if _last_candidate_valid:
		_active_fingerprint = fingerprint


func active_profile_fingerprint() -> String:
	return _active_fingerprint


func last_candidate_valid() -> bool:
	return _last_candidate_valid


func last_candidate_warnings() -> PackedStringArray:
	return _last_candidate_warnings.duplicate()


func stats() -> Dictionary:
	return _merge_candidate_metadata(super.stats())


func _candidate_stats() -> Dictionary:
	return _merge_candidate_metadata(super.stats())


func _merge_candidate_metadata(base: Dictionary) -> Dictionary:
	var out: Dictionary = base.duplicate(true)
	out["candidate_valid"] = _last_candidate_valid
	out["candidate_rejected"] = not _last_candidate_valid
	out["candidate_warnings"] = _last_candidate_warnings.duplicate()
	out["active_fingerprint"] = _active_fingerprint
	out["attempt_fingerprint"] = _last_attempt_fingerprint
	out["attempt_generation"] = _attempt_generation
	out["rejected_candidates"] = _rejected_candidates
	out["last_known_good_preserved"] = not _last_candidate_valid
	return out


func _capture_active_state() -> Dictionary:
	return {
		"headers": _headers.duplicate(),
		"params": _params.duplicate(),
		"code_texture": _code_texture,
		"output_index": _output_index,
		"active": _active,
		"warnings": _warnings.duplicate(),
		"fingerprint": _fingerprint,
		"generation": _compile_generation,
	}


func _restore_active_state(snapshot: Dictionary) -> void:
	_headers = snapshot["headers"]
	_params = snapshot["params"]
	_code_texture = snapshot["code_texture"] as ImageTexture
	_output_index = int(snapshot["output_index"])
	_active = bool(snapshot["active"])
	_warnings = snapshot["warnings"]
	_fingerprint = String(snapshot["fingerprint"])
	_compile_generation = int(snapshot["generation"])


func _validate_candidate(terrain: Resource) -> PackedStringArray:
	var issues := PackedStringArray()
	if terrain == null:
		issues.append("Terrain authoring profile is unavailable; keeping the last valid terrain.")
		return issues

	var slots_value: Variant = terrain.get(&"displacement_slots")
	if not (slots_value is Array):
		issues.append("Terrain displacement slots are malformed; keeping the last valid terrain.")
		return issues

	for slot_value: Variant in slots_value as Array:
		var slot: Resource = slot_value as Resource
		if slot == null or not bool(slot.get(&"enabled")) or int(slot.get(&"domain")) != 0:
			continue
		var graph: Resource = slot.get(&"graph") as Resource
		var label: String = String(slot.get(&"display_name"))
		if label.is_empty():
			label = String(slot.get(&"slot_id"))
		if graph == null:
			issues.append("%s has no terrain-shape graph." % label)
			continue
		_validate_graph(graph, label, issues)
	return issues


func _validate_graph(graph: Resource, label: String, issues: PackedStringArray) -> void:
	var nodes_value: Variant = graph.get(&"nodes")
	var links_value: Variant = graph.get(&"links")
	if not (nodes_value is Array) or not (links_value is Array):
		issues.append("%s has malformed graph data." % label)
		return

	var nodes: Dictionary = {}
	var output_ids := PackedStringArray()
	for node_value: Variant in nodes_value as Array:
		if not (node_value is Dictionary):
			issues.append("%s contains an invalid node record." % label)
			continue
		var node: Dictionary = node_value as Dictionary
		var node_id: String = String(node.get("id", ""))
		if node_id.is_empty():
			issues.append("%s contains a node with no ID." % label)
			continue
		if nodes.has(node_id):
			issues.append("%s contains duplicate node ID %s." % [label, node_id])
			continue
		nodes[node_id] = node
		if String(node.get("type", "")) == "OUTPUT_DISPLACEMENT":
			output_ids.append(node_id)

	if output_ids.size() != 1:
		issues.append("%s must have exactly one Final Terrain output." % label)
		return

	var inputs: Dictionary = {}
	for link_value: Variant in links_value as Array:
		if not (link_value is Dictionary):
			issues.append("%s contains an invalid connection." % label)
			continue
		var link: Dictionary = link_value as Dictionary
		var from_id: String = String(link.get("from", ""))
		var to_id: String = String(link.get("to", ""))
		var to_port: int = int(link.get("to_port", -1))
		if not nodes.has(from_id) or not nodes.has(to_id) or to_port < 0:
			issues.append("%s contains a connection to a missing node." % label)
			continue
		var key: String = "%s:%d" % [to_id, to_port]
		if inputs.has(key):
			issues.append("%s has more than one wire connected to the same input." % label)
			continue
		inputs[key] = from_id

	var output_id: String = output_ids[0]
	var output_source: String = String(inputs.get("%s:0" % output_id, ""))
	if output_source.is_empty():
		issues.append("%s Final Terrain is disconnected. The previous terrain remains active." % label)
		return

	var visiting: Dictionary = {}
	var visited: Dictionary = {}
	_validate_reachable_node(output_source, nodes, inputs, visiting, visited, label, issues)


func _validate_reachable_node(node_id: String, nodes: Dictionary, inputs: Dictionary,
		visiting: Dictionary, visited: Dictionary, label: String,
		issues: PackedStringArray) -> void:
	if visited.has(node_id):
		return
	if visiting.has(node_id):
		issues.append("%s contains a loop near %s." % [label, node_id])
		return
	if not nodes.has(node_id):
		issues.append("%s references missing terrain node %s." % [label, node_id])
		return

	visiting[node_id] = true
	var node: Dictionary = nodes[node_id] as Dictionary
	var node_type: String = String(node.get("type", ""))
	var required_inputs: int = int(REQUIRED_INPUTS_BY_NODE.get(node_type, 0))
	for port: int in required_inputs:
		var source_id: String = String(inputs.get("%s:%d" % [node_id, port], ""))
		if source_id.is_empty():
			issues.append("%s: %s is missing a required input." % [label, _friendly_node_name(node_type)])
			continue
		_validate_reachable_node(source_id, nodes, inputs, visiting, visited, label, issues)

	var parameters_value: Variant = node.get("parameters", {})
	if not _variant_is_finite(parameters_value):
		issues.append("%s: %s contains a non-finite value." % [label, _friendly_node_name(node_type)])

	visiting.erase(node_id)
	visited[node_id] = true


func _variant_is_finite(value: Variant) -> bool:
	match typeof(value):
		TYPE_FLOAT:
			return is_finite(float(value))
		TYPE_VECTOR2:
			var v2: Vector2 = value
			return is_finite(v2.x) and is_finite(v2.y)
		TYPE_VECTOR3:
			var v3: Vector3 = value
			return is_finite(v3.x) and is_finite(v3.y) and is_finite(v3.z)
		TYPE_VECTOR4:
			var v4: Vector4 = value
			return is_finite(v4.x) and is_finite(v4.y) and is_finite(v4.z) and is_finite(v4.w)
		TYPE_COLOR:
			var color: Color = value
			return is_finite(color.r) and is_finite(color.g) and is_finite(color.b) and is_finite(color.a)
		TYPE_ARRAY:
			for item: Variant in value as Array:
				if not _variant_is_finite(item):
					return false
			return true
		TYPE_DICTIONARY:
			for item: Variant in (value as Dictionary).values():
				if not _variant_is_finite(item):
					return false
			return true
	return true


func _friendly_node_name(node_type: String) -> String:
	match node_type:
		"OUTPUT_DISPLACEMENT": return "Final Terrain"
		"RIDGED_MOUNTAINS": return "Mountain Shape"
		"EROSION_CHANNELS": return "Valleys & Erosion"
		"SEDIMENT_DEPOSIT": return "Sediment"
		"NOISE_LAYER": return "Terrain Detail"
		"MULTIPLY": return "Strength"
		"DIVIDE": return "Divide"
		"ADD": return "Combine Terrain"
		"SUBTRACT": return "Subtract Terrain"
		"MIX": return "Terrain Blend"
	return node_type.replace("_", " ").capitalize()
