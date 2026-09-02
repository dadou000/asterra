extends Node
## Phase 45 regression for preset recipes -> ordinary guided graphs -> Phase 41 runtime.

const TERRAIN := preload("res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const SLOT := preload("res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const GUIDED := preload("res://scripts/world_authoring/model/terrain_guided_feature_graph.gd")
const PRESETS := preload("res://scripts/world_authoring/model/terrain_feature_preset_catalog.gd")
const RUNTIME := preload("res://scripts/world_authoring/terrain_displacement_runtime_phase41.gd")


func _ready() -> void:
	var error: String = _validate()
	if not error.is_empty():
		push_error("TERRAIN_PRESETS_PHASE45_FAILED: " + error)
		get_tree().quit(1)
		return
	print("TERRAIN_PRESETS_PHASE45_OK: all curated presets lower to editable guided graphs and the authoritative displacement runtime")
	get_tree().quit(0)


func _validate() -> String:
	if PRESETS.IDS.size() != 6:
		return "expected six curated presets"
	var labels: Dictionary = {}
	for preset_id: String in PRESETS.IDS:
		var preset_label: String = PRESETS.label(preset_id)
		if preset_label.is_empty() or labels.has(preset_label):
			return "preset labels are empty or duplicated"
		labels[preset_label] = true
		var specs: Array[Dictionary] = PRESETS.specs(preset_id, 3)
		if specs.is_empty() or specs.size() != PRESETS.feature_count(preset_id):
			return "preset %s has no stable feature recipe" % preset_id
		var terrain: Resource = TERRAIN.new()
		terrain.call("ensure_valid")
		for spec: Dictionary in specs:
			var slot: Resource = terrain.call("create_shader_slot", SLOT.Domain.DISPLACEMENT,
				String(spec.get("name", "CI Preset Feature"))) as Resource
			if slot == null:
				return "preset %s could not create a displacement slot" % preset_id
			slot.set(&"enabled", true)
			slot.set(&"strength", 1.0)
			var graph: Resource = slot.get(&"graph") as Resource
			var config: Dictionary = spec.get("config", {}) as Dictionary
			if graph == null or not GUIDED.rebuild(graph, config):
				return "preset %s could not build guided graph %s" % [preset_id, String(spec.get("name", ""))]
			if not GUIDED.is_guided_graph(graph):
				return "preset %s created a non-guided graph" % preset_id
			var round_trip: Dictionary = GUIDED.config_from_graph(graph)
			if String(round_trip.get("effect_kind", "")) != String(config.get("effect_kind", "")) \
					or String(round_trip.get("area_kind", "")) != String(config.get("area_kind", "")):
				return "preset %s did not round-trip effect/location metadata" % preset_id

		var runtime: Node = RUNTIME.new() as Node
		var compiled: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
		if not bool(compiled.get("candidate_valid", false)) or not bool(compiled.get("active", false)):
			runtime.free()
			return "preset %s was rejected by the authoritative runtime" % preset_id
		for sample: Vector2 in [Vector2(0.0, 0.0), Vector2(6.2, 0.0), Vector2(15.0, 15.0), Vector2(-30.0, 179.0)]:
			var value: float = float(runtime.call("evaluate_height",
				_direction(sample.x, sample.y), 0.0, 0, 0, 0.0, 0.0))
			if not is_finite(value):
				runtime.free()
				return "preset %s produced non-finite displacement" % preset_id
		runtime.free()

	# Multi-part crater semantics: deep center, genuinely raised rim, zero outside.
	var crater_terrain: Resource = TERRAIN.new()
	crater_terrain.call("ensure_valid")
	var crater_slots: Array[Resource] = []
	for spec: Dictionary in PRESETS.specs(PRESETS.CRATER, 0):
		var slot: Resource = crater_terrain.call("create_shader_slot", SLOT.Domain.DISPLACEMENT,
			String(spec.get("name", "Crater"))) as Resource
		slot.set(&"enabled", true)
		var graph: Resource = slot.get(&"graph") as Resource
		if graph == null or not GUIDED.rebuild(graph, spec.get("config", {}) as Dictionary):
			return "crater recipe could not build"
		crater_slots.append(slot)
	var crater_runtime: Node = RUNTIME.new() as Node
	var crater_compile: Dictionary = crater_runtime.call("compile_from_terrain", crater_terrain) as Dictionary
	if not bool(crater_compile.get("candidate_valid", false)):
		crater_runtime.free()
		return "combined crater recipe did not compile"
	var center: float = float(crater_runtime.call("evaluate_height", _direction(0.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	var rim: float = float(crater_runtime.call("evaluate_height", _direction(0.0, 6.2), 0.0, 0, 0, 0.0, 0.0))
	var outside: float = float(crater_runtime.call("evaluate_height", _direction(0.0, 10.0), 0.0, 0, 0, 0.0, 0.0))
	if center > -300.0 or rim < 200.0 or absf(outside) > 0.01:
		crater_runtime.free()
		return "crater preset no longer forms depressed center + raised rim + clean exterior"

	# Presets remain ordinary editable guided graphs after creation.
	var basin_graph: Resource = crater_slots[0].get(&"graph") as Resource
	if basin_graph == null or not GUIDED.set_config_value(basin_graph, "amount_m", -500.0):
		crater_runtime.free()
		return "preset-created feature is not editable through guided config"
	crater_compile = crater_runtime.call("compile_from_terrain", crater_terrain) as Dictionary
	center = float(crater_runtime.call("evaluate_height", _direction(0.0, 0.0), 0.0, 0, 0, 0.0, 0.0))
	if not bool(crater_compile.get("candidate_valid", false)) or center > -490.0:
		crater_runtime.free()
		return "editing a preset-created feature did not reach the runtime"
	crater_runtime.free()

	# Seed salt changes procedural presets without changing their authored shape recipe.
	var mountain_a: Dictionary = PRESETS.specs(PRESETS.MOUNTAIN_RANGE, 0)[0].get("config", {}) as Dictionary
	var mountain_b: Dictionary = PRESETS.specs(PRESETS.MOUNTAIN_RANGE, 1)[0].get("config", {}) as Dictionary
	if int(mountain_a.get("seed", -1)) == int(mountain_b.get("seed", -1)):
		return "preset seed salt does not vary repeated procedural presets"
	if float(mountain_a.get("amount_m", 0.0)) != float(mountain_b.get("amount_m", 1.0)):
		return "seed salt changed non-seed preset geometry"
	return ""


func _direction(latitude_deg: float, longitude_deg: float) -> Vector3:
	var latitude_rad: float = deg_to_rad(latitude_deg)
	var longitude_rad: float = deg_to_rad(longitude_deg)
	var horizontal: float = cos(latitude_rad)
	return Vector3(horizontal * sin(longitude_rad), sin(latitude_rad),
		horizontal * cos(longitude_rad))
