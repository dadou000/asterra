extends Node
## Hard gate for splitting PRODUCTION_GENERATED_HEIGHT into editable graph stages.
##
## Untouched authoring must remain a literal identity view of the resident production
## terrain. This test verifies the semantic schema, graph defaults, shader
## declarations/order/operations, and most importantly that the canonical production
## Shape graph emits ZERO authored displacement instructions.

const SCHEMA := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_schema.gd")
const GRAPH := preload(
	"res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
const TERRAIN_PROFILE := preload(
	"res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const SLOT := preload(
	"res://scripts/world_authoring/model/terrain_shader_slot_definition.gd")
const RUNTIME := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase34.gd")

const GEOMORPH_SHADER_PATH := "res://shaders/gpu_geomorph.gdshaderinc"
const CACHED_SHADER_PATH := "res://shaders/spherical_geometry_clipmap_cached_surface.gdshader"
const PRODUCTION_SHAPE_SLOT_ID := "production-terrain-shape"
const EXPECTED_STAGE_ORDER := "broad|mountain|mid|channel|deposit|fine|dune|micro|glacial"
const EXPECTED_DEFAULT_GUARD_M: float = 533.4


func _ready() -> void:
	var error: String = _validate_all()
	if not error.is_empty():
		push_error("PRODUCTION_GEOMORPH_EQUIVALENCE_FAILED: " + error)
		get_tree().quit(1)
		return
	print("PRODUCTION_GEOMORPH_EQUIVALENCE_OK: untouched graph is bytecode-free and schema matches production shader")
	get_tree().quit(0)


func _validate_all() -> String:
	print("PRODUCTION_GEOMORPH_EQUIVALENCE_STEP: schema")
	var schema_errors: PackedStringArray = SCHEMA.validate_schema()
	if not schema_errors.is_empty():
		return "schema invalid: %s" % "; ".join(schema_errors)
	var actual_order: String = "|".join(SCHEMA.ordered_stage_ids())
	if actual_order != EXPECTED_STAGE_ORDER:
		return "production stage order changed: %s" % actual_order

	print("PRODUCTION_GEOMORPH_EQUIVALENCE_STEP: semantics")
	var semantic_error: String = _validate_stage_semantics()
	if not semantic_error.is_empty():
		return semantic_error

	print("PRODUCTION_GEOMORPH_EQUIVALENCE_STEP: graph_defaults")
	var defaults_error: String = _validate_graph_defaults()
	if not defaults_error.is_empty():
		return defaults_error

	print("PRODUCTION_GEOMORPH_EQUIVALENCE_STEP: shader_contract")
	var shader_error: String = _validate_shader_contract()
	if not shader_error.is_empty():
		return shader_error

	print("PRODUCTION_GEOMORPH_EQUIVALENCE_STEP: identity_runtime")
	var identity_error: String = _validate_identity_runtime()
	if not identity_error.is_empty():
		return identity_error

	var guard: float = SCHEMA.production_guard_m(SCHEMA.control_defaults())
	if not is_equal_approx(guard, EXPECTED_DEFAULT_GUARD_M):
		return "canonical default guard changed: expected %.3f m, got %.3f m" \
			% [EXPECTED_DEFAULT_GUARD_M, guard]
	return ""


func _validate_stage_semantics() -> String:
	var by_id: Dictionary = {}
	for stage: Dictionary in SCHEMA.stage_specs():
		by_id[String(stage.get("id", ""))] = stage
	if String((by_id.get("channel", {}) as Dictionary).get("operation", "")) != "subtract_positive":
		return "channel stage is no longer subtractive"
	var deposit: Dictionary = by_id.get("deposit", {}) as Dictionary
	if String(deposit.get("operation", "")) != "add_positive" \
			or String(deposit.get("parent_stage", "")) != "channel":
		return "deposition no longer preserves its nested channel-sample semantics"
	var dune: Dictionary = by_id.get("dune", {}) as Dictionary
	if String(dune.get("parent_stage", "")) != "fine":
		return "dunes no longer preserve the production fine-detail branch"
	var micro: Dictionary = by_id.get("micro", {}) as Dictionary
	if not (micro.get("dependencies", []) as Array).has("fine_strength"):
		return "micro relief lost its production dependency on fine_strength"
	var glacial: Dictionary = by_id.get("glacial", {}) as Dictionary
	if String(glacial.get("operation", "")) != "mix_accumulated":
		return "glacial shaping must transform accumulated height, not become additive"
	return ""


func _validate_graph_defaults() -> String:
	var graph_defaults: Dictionary = GRAPH.production_control_defaults(
		"PRODUCTION_GEOMORPH_SETTINGS")
	var schema_defaults: Dictionary = SCHEMA.control_defaults()
	if graph_defaults.size() != schema_defaults.size():
		return "graph/schema control count mismatch: %d vs %d" \
			% [graph_defaults.size(), schema_defaults.size()]
	for key_value: Variant in schema_defaults.keys():
		var key: String = String(key_value)
		if not graph_defaults.has(key):
			return "graph defaults are missing control %s" % key
		if not _variant_equal(graph_defaults[key], schema_defaults[key]):
			return "graph default %s drifted: graph=%s schema=%s" \
				% [key, str(graph_defaults[key]), str(schema_defaults[key])]
	for key_value: Variant in graph_defaults.keys():
		var key: String = String(key_value)
		if not schema_defaults.has(key):
			return "graph exposes unmodeled production control %s" % key
	return ""


func _validate_shader_contract() -> String:
	var geomorph: String = FileAccess.get_file_as_string(GEOMORPH_SHADER_PATH)
	var cached: String = FileAccess.get_file_as_string(CACHED_SHADER_PATH)
	if geomorph.is_empty():
		return "cannot read %s" % GEOMORPH_SHADER_PATH
	if cached.is_empty():
		return "cannot read %s" % CACHED_SHADER_PATH

	# Stage anchors must occur in exact semantic order. Operation anchors and seed
	# offsets must live inside their stage range so moving an operation into another
	# branch cannot silently pass this gate.
	var stages: Array[Dictionary] = SCHEMA.stage_specs()
	var cursor: int = -1
	for index: int in stages.size():
		var stage: Dictionary = stages[index]
		var stage_id: String = String(stage.get("id", ""))
		var anchor: String = String(stage.get("anchor", ""))
		var anchor_pos: int = geomorph.find(anchor, cursor + 1)
		if anchor_pos < 0:
			return "shader is missing %s stage anchor: %s" % [stage_id, anchor]
		if anchor_pos <= cursor:
			return "shader stage order changed at %s" % stage_id
		var next_boundary: int = geomorph.length()
		if index + 1 < stages.size():
			var next_anchor: String = String(stages[index + 1].get("anchor", ""))
			next_boundary = geomorph.find(next_anchor, anchor_pos + anchor.length())
			if next_boundary < 0:
				return "shader is missing next-stage anchor after %s" % stage_id
		var operation_anchor: String = String(stage.get("operation_anchor", ""))
		var operation_pos: int = geomorph.find(operation_anchor, anchor_pos)
		if operation_pos < anchor_pos or operation_pos >= next_boundary:
			return "%s operation moved outside its canonical shader stage" % stage_id
		for seed_value: Variant in stage.get("seed_offsets", []) as Array:
			var seed_anchor: String = "seed + %du" % int(seed_value)
			var seed_pos: int = geomorph.find(seed_anchor, anchor_pos)
			if seed_pos < anchor_pos or seed_pos >= next_boundary:
				return "%s seed offset %d moved outside its canonical stage" \
					% [stage_id, int(seed_value)]
		cursor = anchor_pos

	var defaults: Dictionary = SCHEMA.control_defaults()
	for key_value: Variant in defaults.keys():
		var key: String = String(key_value)
		var uniform_name: String = SCHEMA.uniform_for_control(key)
		if uniform_name.is_empty():
			continue # override_seed is CPU-side policy, not a shader uniform.
		var source: String = cached if SCHEMA.shader_source_for_control(key) == "cached_surface" else geomorph
		var shader_default: Variant = _read_uniform_default(source, uniform_name)
		if shader_default == null:
			return "shader declaration missing for %s (%s)" % [key, uniform_name]
		if not _variant_equal(shader_default, defaults[key]):
			return "shader default %s drifted: shader=%s schema=%s" \
				% [uniform_name, str(shader_default), str(defaults[key])]

	if cached.find("* coast_guard * u_detail_strength;") < 0:
		return "cached terrain no longer applies coast_guard * detail_strength after geomorph"
	return ""


func _validate_identity_runtime() -> String:
	var terrain: Resource = TERRAIN_PROFILE.new()
	terrain.call("ensure_valid")
	var slot: Resource = terrain.call("create_shader_slot",
		SLOT.Domain.DISPLACEMENT, "Base Terrain Shape") as Resource
	if slot == null:
		return "could not create canonical production displacement slot"
	slot.set(&"slot_id", PRODUCTION_SHAPE_SLOT_ID)
	slot.set(&"enabled", true)
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return "canonical production displacement slot has no graph"
	graph.call("create_production_stage_graph", GRAPH.Domain.DISPLACEMENT)

	var settings: Dictionary = {}
	for node_value: Variant in graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		if String(node.get("type", "")) == "PRODUCTION_GEOMORPH_SETTINGS":
			settings = (node.get("parameters", {}) as Dictionary).duplicate(true)
			break
	if settings.is_empty():
		return "canonical production graph has no geomorph settings node"
	if settings.size() != SCHEMA.control_defaults().size():
		return "canonical production graph settings do not match schema size"

	var runtime: Node = RUNTIME.new() as Node
	var stats: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	var active: bool = bool(stats.get("active", false))
	var instructions: int = int(stats.get("instructions", 0))
	var envelope: Dictionary = stats.get("displacement_envelope", {}) as Dictionary
	runtime.free()
	if active or instructions != 0:
		return "untouched production identity emitted authored bytecode (active=%s instructions=%d)" \
			% [str(active), instructions]
	if not bool(envelope.get("bounds_known", false)):
		return "untouched production identity unexpectedly has unknown displacement bounds"
	if not is_equal_approx(float(envelope.get("production_max_abs_m", 0.0)), EXPECTED_DEFAULT_GUARD_M):
		return "identity runtime production guard drifted to %.3f m" \
			% float(envelope.get("production_max_abs_m", 0.0))
	return ""


func _read_uniform_default(source: String, uniform_name: String) -> Variant:
	var name_pos: int = source.find(uniform_name)
	if name_pos < 0:
		return null
	var line_start: int = source.rfind("\n", name_pos)
	line_start = 0 if line_start < 0 else line_start + 1
	var line_end: int = source.find("\n", name_pos)
	line_end = source.length() if line_end < 0 else line_end
	var line: String = source.substr(line_start, line_end - line_start)
	if not line.contains("uniform "):
		return null
	var equals_pos: int = line.rfind("=")
	var semicolon_pos: int = line.find(";", equals_pos)
	if equals_pos < 0 or semicolon_pos < 0:
		return null
	var raw: String = line.substr(equals_pos + 1, semicolon_pos - equals_pos - 1).strip_edges()
	if line.contains("uniform int "):
		return raw.to_int()
	if line.contains("uniform float "):
		return raw.to_float()
	return null


func _variant_equal(a: Variant, b: Variant) -> bool:
	if (a is float or a is int) and (b is float or b is int):
		return is_equal_approx(float(a), float(b))
	return a == b
