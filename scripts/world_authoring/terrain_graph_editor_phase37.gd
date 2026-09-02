extends "res://scripts/world_authoring/terrain_graph_editor_phase36.gd"
## Phase 37: structurally expose the existing production geomorph shader.
##
## The opaque PRODUCTION_GENERATED_HEIGHT identity is migrated, only when untouched,
## into native stage nodes backed by TerrainProductionGeomorphSchema. These are a
## presentation/parameter view over the resident production GPU shader: canonical
## topology still emits zero authored bytecode. Rewiring an intrinsic stage is
## visible in the editor but the Phase 35 runtime rejects it until exact structural
## lowering exists, so the rendered planet remains last-known-good.

const NATIVE_GRAPH := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")
const PRODUCTION_SCHEMA := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_schema.gd")
const GUIDED_CATALOG := preload(
	"res://scripts/world_authoring/model/terrain_beginner_parameter_catalog.gd")

const PARAM_RANGES: Dictionary = {
	"broad_strength":[0.0, 4.0, 0.01],
	"broad_wavelength_m":[1.0, 100000.0, 1.0],
	"broad_low_amplitude_m":[0.0, 2000.0, 0.1],
	"broad_mountain_amplitude_m":[0.0, 4000.0, 0.1],
	"broad_warp":[0.0, 4.0, 0.01],
	"mountain_strength":[0.0, 4.0, 0.01],
	"mountain_wavelength_m":[1.0, 100000.0, 1.0],
	"mountain_amplitude_m":[0.0, 4000.0, 0.1],
	"mountain_warp":[0.0, 4.0, 0.01],
	"mountain_ridge_scale":[0.05, 8.0, 0.01],
	"mountain_cell_mix":[0.0, 1.0, 0.01],
	"mid_strength":[0.0, 4.0, 0.01],
	"mid_wavelength_m":[1.0, 50000.0, 1.0],
	"mid_ridge_amplitude_m":[0.0, 2000.0, 0.1],
	"mid_noise_amplitude_m":[0.0, 1000.0, 0.1],
	"mid_warp":[0.0, 4.0, 0.01],
	"mid_ridge_scale":[0.05, 8.0, 0.01],
	"mid_detail_scale":[0.05, 8.0, 0.01],
	"channel_strength":[0.0, 4.0, 0.01],
	"channel_wavelength_m":[1.0, 10000.0, 1.0],
	"channel_depth_min_m":[0.0, 1000.0, 0.1],
	"channel_depth_max_m":[0.0, 2000.0, 0.1],
	"channel_warp":[0.0, 4.0, 0.01],
	"channel_power":[0.1, 12.0, 0.01],
	"flow_along_scale":[0.01, 4.0, 0.01],
	"flow_across_scale":[0.01, 4.0, 0.01],
	"deposit_strength":[0.0, 4.0, 0.01],
	"deposit_amplitude_min_m":[0.0, 500.0, 0.1],
	"deposit_amplitude_max_m":[0.0, 1000.0, 0.1],
	"deposit_scale":[0.01, 4.0, 0.01],
	"deposit_power":[0.1, 8.0, 0.01],
	"fine_strength":[0.0, 4.0, 0.01],
	"fine_wavelength_m":[0.5, 5000.0, 0.5],
	"fine_amplitude_m":[0.0, 250.0, 0.05],
	"dune_strength":[0.0, 4.0, 0.01],
	"dune_wavelength_m":[0.5, 5000.0, 0.5],
	"dune_amplitude_m":[0.0, 500.0, 0.05],
	"dune_warp":[0.0, 4.0, 0.01],
	"micro_wavelength_m":[0.25, 1000.0, 0.25],
	"micro_amplitude_m":[0.0, 100.0, 0.01],
	"glacial_strength":[0.0, 4.0, 0.01],
	"glacial_wavelength_m":[1.0, 50000.0, 1.0],
	"glacial_amplitude_m":[0.0, 2000.0, 0.1],
	"glacial_base_scale":[0.0, 2.0, 0.01],
	"glacial_mix":[0.0, 2.0, 0.01],
}


func setup(session: RefCounted, slot: Resource,
		rebuild_requested: Callable = Callable()) -> void:
	var graph: Resource = slot.get(&"graph") as Resource if slot != null else null
	if slot != null and String(slot.get(&"slot_id")) == NATIVE_GRAPH.PRODUCTION_SHAPE_SLOT_ID \
			and NATIVE_GRAPH.is_legacy_identity_graph(graph):
		if session != null and session.has_method("stage_action"):
			session.call("stage_action", "Expose production geomorph stages", func() -> void:
				NATIVE_GRAPH.migrate_legacy_identity(graph)
			, 2)
		else:
			NATIVE_GRAPH.migrate_legacy_identity(graph)
	super.setup(session, slot, rebuild_requested)


func _graph_node_title(node_type: String, node_data: Dictionary) -> String:
	if node_type == NATIVE_GRAPH.START_TYPE:
		return "PRODUCTION · GEOMORPH START"
	if node_type == NATIVE_GRAPH.COMPOSE_TYPE:
		return "PRODUCTION · MACRO + DETAIL"
	if node_type == NATIVE_GRAPH.SETTINGS_TYPE and _graph != null \
			and NATIVE_GRAPH.has_structural_nodes(_graph):
		return "PRODUCTION · GLOBAL GEOMORPH CONTEXT"
	var stage_title: String = NATIVE_GRAPH.stage_title_for_node_type(node_type)
	if not stage_title.is_empty():
		return "PRODUCTION · " + stage_title.to_upper()
	return super._graph_node_title(node_type, node_data)


func _create_graph_node(node_data: Dictionary) -> void:
	var node_type: String = String(node_data.get("type", ""))
	var structural: bool = _graph != null and NATIVE_GRAPH.has_structural_nodes(_graph)
	if not structural or (node_type != NATIVE_GRAPH.START_TYPE \
			and node_type != NATIVE_GRAPH.COMPOSE_TYPE \
			and node_type != NATIVE_GRAPH.SETTINGS_TYPE \
			and not node_type.begins_with(NATIVE_GRAPH.STAGE_PREFIX)):
		super._create_graph_node(node_data)
		return

	var node_id: String = String(node_data.get("id", ""))
	if node_id.is_empty():
		return
	var graph_node := GraphNode.new()
	graph_node.name = node_id
	graph_node.title = _graph_node_title(node_type, node_data)
	graph_node.position_offset = Vector2(node_data.get("position", Vector2.ZERO))
	graph_node.resizable = false
	graph_node.ignore_invalid_connection_type = true
	_graph_edit.add_child(graph_node)
	var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary

	if node_type == NATIVE_GRAPH.START_TYPE:
		_add_port_row(graph_node, "Native detail accumulator · 0 m", false, true)
		_add_settings_note(graph_node,
			"Starts the exact gm_geomorph_height accumulation. World context, macro height, geology, climate and hydrology remain renderer-owned inputs.")
		return
	if node_type == NATIVE_GRAPH.COMPOSE_TYPE:
		_add_port_row(graph_node, "Geomorph detail → generated height", true, true)
		_add_settings_note(graph_node,
			"Applies the production coast guard and Overall Detail, then adds macro elevation. This is the exact macro_h + procedural_detail boundary.")
		return
	if node_type == NATIVE_GRAPH.SETTINGS_TYPE:
		_add_settings_note(graph_node,
			"Global context shared by the native stages. Stage-specific controls now live on the stage that actually uses them.")
		_add_float_setting(graph_node, node_id, parameters,
			"Overall detail", "detail_strength", 1.0, 0.0, 4.0, 0.01)
		_add_float_setting(graph_node, node_id, parameters,
			"Domain warp", "warp_strength", 1.0, 0.0, 4.0, 0.01)
		_add_bool_setting(graph_node, node_id, parameters,
			"Override detail seed", "override_seed", false)
		_add_float_setting(graph_node, node_id, parameters,
			"Detail seed", "detail_seed", 1337.0, 1.0, 2147483000.0, 1.0)
		return

	_add_port_row(graph_node, "Accumulated native detail", true, true)
	var spec: Dictionary = NATIVE_GRAPH.stage_spec_for_node_type(node_type)
	_add_settings_note(graph_node, _stage_semantic_note(spec))
	for key_value: Variant in spec.get("parameters", []) as Array:
		var key: String = String(key_value)
		_add_native_parameter(graph_node, node_id, parameters, key)


func _stage_semantic_note(spec: Dictionary) -> String:
	var operation: String = String(spec.get("operation", ""))
	var note: String = "Exact resident shader stage. "
	match operation:
		"subtract_positive": note += "Subtracts positive incision from accumulated detail."
		"add_positive": note += "Adds positive deposition to accumulated detail."
		"mix_accumulated": note += "Terminal mix that reshapes the accumulated detail; it is not an additive layer."
		_: note += "Adds a signed production contribution to accumulated detail."
	var parent: String = String(spec.get("parent_stage", ""))
	if not parent.is_empty():
		note += " Shares the %s production branch/sample." % parent.capitalize()
	var dependencies: Array = spec.get("dependencies", []) as Array
	if not dependencies.is_empty():
		var names := PackedStringArray()
		for dependency: Variant in dependencies:
			names.append(String(dependency).replace("_", " "))
		note += " Also depends on %s." % ", ".join(names)
	return note


func _add_native_parameter(node: GraphNode, node_id: String, parameters: Dictionary,
		key: String) -> void:
	var fallback: float = float(PRODUCTION_SCHEMA.CONTROL_DEFAULTS.get(key, 0.0))
	var range_value: Variant = PARAM_RANGES.get(key, [-1000000.0, 1000000.0, 0.01])
	var range: Array = range_value as Array
	_add_float_setting(node, node_id, parameters, _parameter_label(key), key,
		fallback, float(range[0]), float(range[1]), float(range[2]))


func _parameter_label(key: String) -> String:
	var text: String = key.replace("_", " ").capitalize()
	text = text.replace(" M", " (m)")
	return text


func _add_guided_control(spec: Dictionary, _parameters: Dictionary) -> void:
	# Simple/Detailed are still alternate views of this same graph. Resolve every
	# key to its new owning native stage so guided edits cannot create a shadow copy
	# of stage parameters inside the global settings node.
	var key: String = String(spec.get("key", ""))
	var owner_type: String = NATIVE_GRAPH.owner_node_type_for_control(key)
	var owner_id: String = _find_node_of_type(owner_type)
	if owner_id.is_empty():
		super._add_guided_control(spec, _parameters)
		return
	var owner_data: Dictionary = _node_data(owner_id)
	var parameters: Dictionary = owner_data.get("parameters", {}) as Dictionary

	var card := VBoxContainer.new()
	card.custom_minimum_size = Vector2(650.0, 72.0)
	card.add_theme_constant_override("separation", 2)
	_guided_content.add_child(card)
	var top := HBoxContainer.new()
	card.add_child(top)
	var title := Label.new()
	title.text = String(spec.get("title", "Terrain"))
	title.custom_minimum_size.x = 215.0
	title.tooltip_text = String(spec.get("description", ""))
	top.add_child(title)

	var minimum: float = float(spec.get("min", 0.0))
	var maximum: float = float(spec.get("max", 1.0))
	var step: float = float(spec.get("step", 0.01))
	var default_value: float = float(spec.get("default", 0.0))
	var current: float = clampf(float(parameters.get(key, default_value)), minimum, maximum)
	var slider := HSlider.new()
	slider.min_value = minimum
	slider.max_value = maximum
	slider.step = step
	slider.value = current
	slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	slider.custom_minimum_size.x = 260.0
	top.add_child(slider)
	var value := SpinBox.new()
	value.min_value = minimum
	value.max_value = maximum
	value.step = step
	value.value = current
	value.custom_minimum_size.x = 125.0
	value.allow_greater = false
	value.allow_lesser = false
	var unit: String = String(spec.get("unit", ""))
	if not unit.is_empty():
		value.suffix = " " + unit
	top.add_child(value)
	slider.value_changed.connect(func(next_value: float) -> void:
		value.set_value_no_signal(next_value)
		_set_node_parameter(owner_id, key, next_value)
	)
	value.value_changed.connect(func(next_value: float) -> void:
		slider.set_value_no_signal(next_value)
		_set_node_parameter(owner_id, key, next_value)
	)
	var description := Label.new()
	description.text = String(spec.get("description", ""))
	description.modulate = Color(0.57, 0.66, 0.73)
	description.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	description.custom_minimum_size.x = 620.0
	card.add_child(description)


func _reset_visible_guided_controls() -> void:
	if _session == null or _graph == null:
		return
	var specs: Array[Dictionary] = GUIDED_CATALOG.controls_for_mode(_editing_mode)
	_session.call("stage_action", "Reset terrain shape controls", func() -> void:
		for spec: Dictionary in specs:
			var key: String = String(spec.get("key", ""))
			var owner_type: String = NATIVE_GRAPH.owner_node_type_for_control(key)
			var owner_id: String = _find_node_of_type(owner_type)
			if not owner_id.is_empty():
				_graph.call("set_node_parameter", owner_id, key,
					float(spec.get("default", 0.0)))
	, 2)
	_rebuild_guided_controls()
