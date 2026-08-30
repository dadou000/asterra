class_name WorldAuthoringLiveEditorPhase18
extends "res://scripts/world_authoring/world_authoring_editor_live_phase17.gd"
## Phase 18: expose the values and shaders the renderer is actually using.
##
## The high-level authoring model remains the preferred way to build terrain, but
## Planet Studio now also has a direct advanced inspector. It reads the live
## ShaderMaterial instances owned by the production terrain/ocean/sky renderers,
## shows their real file-backed shader and uniform values, and allows explicit
## per-body overrides. Overrides are re-applied after renderer-owned dynamic
## uniforms so the value displayed as overridden is the value the GPU receives.

const RUNTIME_GENERATION_FIELDS: Array[String] = [
	"detail_amplitude",
	"detail_octaves",
	"detail_base_frequency",
	"quadtree_max_depth",
	"chunk_grid",
	"lod_split_factor",
	"lod_target_error_px",
	"lod_collapse_ratio",
	"collision_depth",
	"collision_stream_depth",
	"collision_stream_radius",
	"collision_grid",
	"edit_cell_size",
	"use_gpu_bake",
]
const MIRRORED_GENERATION_FIELDS: Array[String] = [
	"planet_radius",
	"axial_tilt_deg",
	"atmosphere_height",
]
const BASIC_GENERATION_FIELDS: Array[String] = [
	"world_seed",
	"ocean_fraction",
	"max_uplift",
	"detail_amplitude",
]

const TARGET_TERRAIN := "terrain_ground"
const TARGET_OCEAN_LOCAL := "ocean_local"
const TARGET_OCEAN_ORBIT := "ocean_orbit"
const TARGET_ATMOSPHERE := "atmosphere_sky"
const AUTHORED_WATER_PREFIX := "authored_water:"
const OVERRIDE_REAPPLY_INTERVAL_S: float = 0.0 # every frame; renderer uniforms are dynamic

var _show_actual_generation_parameters: bool = false
var _expanded_shader_targets: Dictionary = {}
var _default_shader_paths: Dictionary = {}
var _uniform_name_cache: Dictionary = {}
var _override_reapply_accum: float = 0.0


func _ready() -> void:
	super._ready()
	# Terrain/ocean nodes run around priorities 9-11. Run the authoring override
	# pass afterwards so explicit overrides win over their per-frame sync uniforms.
	process_priority = 100
	_apply_all_runtime_shader_state()


func _process(delta: float) -> void:
	super._process(delta)
	_override_reapply_accum += delta
	if OVERRIDE_REAPPLY_INTERVAL_S <= 0.0 \
			or _override_reapply_accum >= OVERRIDE_REAPPLY_INTERVAL_S:
		_override_reapply_accum = 0.0
		_apply_all_runtime_shader_state()


func _build_terrain_page() -> void:
	super._build_terrain_page()
	if _world_host == null:
		return
	_build_actual_generation_parameters()
	_build_runtime_shader_inspector(
		TARGET_TERRAIN,
		"Ground terrain — actual render shader",
		_runtime_material(TARGET_TERRAIN))


func _build_water_page() -> void:
	super._build_water_page()
	if _world_host == null:
		return
	_build_runtime_shader_inspector(
		TARGET_OCEAN_LOCAL,
		"Local/regional ocean — actual render shader",
		_runtime_material(TARGET_OCEAN_LOCAL))
	_build_runtime_shader_inspector(
		TARGET_OCEAN_ORBIT,
		"Orbital ocean — actual render shader",
		_runtime_material(TARGET_OCEAN_ORBIT))
	_build_selected_authored_water_shader_inspector()


func _build_atmosphere_page() -> void:
	super._build_atmosphere_page()
	if _world_host == null:
		return
	_build_runtime_shader_inspector(
		TARGET_ATMOSPHERE,
		"Atmosphere + volumetric clouds — actual sky shader",
		_runtime_material(TARGET_ATMOSPHERE))


func _build_actual_generation_parameters() -> void:
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		return
	var generation: Resource = terrain.call("ensure_generation_profile") as Resource
	if generation == null:
		return
	_section("Actual generator configuration")
	var summary := HBoxContainer.new()
	summary.add_theme_constant_override("separation", 8)
	_workspace.add_child(summary)
	var toggle := Button.new()
	toggle.text = "Hide all parameters" if _show_actual_generation_parameters \
		else "Show all actual parameters"
	toggle.pressed.connect(func() -> void:
		_show_actual_generation_parameters = not _show_actual_generation_parameters
		_refresh_current_category()
	)
	summary.add_child(toggle)
	var cfg: Resource = _world_host.get("cfg") as Resource
	var count := _script_variable_count(generation)
	var count_label := Label.new()
	count_label.text = "%d stored generator values • staged value / live GenConfig" % count
	count_label.modulate = Color(0.58, 0.70, 0.79)
	summary.add_child(count_label)
	_add_note("These are the real fields copied into GenConfig. Detail/LOD/runtime fields apply without PlanetBake; macro terrain, geology, erosion and climate fields are correctly marked for a full generator rebuild. Radius, axial tilt and atmosphere height are shown here as mirrors but are edited from their canonical Planet/Atmospheric pages.")
	if not _show_actual_generation_parameters:
		return

	var live_names := _property_name_set(cfg) if cfg != null else {}
	for property: Dictionary in generation.get_property_list():
		if (int(property.get("usage", 0)) & PROPERTY_USAGE_SCRIPT_VARIABLE) == 0:
			continue
		var property_name := String(property.get("name", ""))
		if property_name.is_empty():
			continue
		_build_generation_property_row(
			generation,
			cfg,
			live_names,
			property_name,
			generation.get(property_name))


func _build_generation_property_row(generation: Resource, cfg: Resource,
		live_names: Dictionary, property_name: String, staged_value: Variant) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	_workspace.add_child(row)
	var label := Label.new()
	label.text = property_name
	label.tooltip_text = "GenerationAuthoringProfile.%s" % property_name
	label.custom_minimum_size.x = 205.0
	row.add_child(label)

	var mirrored := property_name in MIRRORED_GENERATION_FIELDS
	var value_type := typeof(staged_value)
	if value_type == TYPE_BOOL:
		var toggle := CheckButton.new()
		toggle.button_pressed = bool(staged_value)
		toggle.disabled = mirrored
		toggle.toggled.connect(_on_generation_bool_changed.bind(generation, property_name))
		row.add_child(toggle)
	elif value_type == TYPE_INT:
		var line := LineEdit.new()
		line.text = str(int(staged_value))
		line.custom_minimum_size.x = 150.0
		line.editable = not mirrored
		line.text_submitted.connect(_on_generation_int_submitted.bind(generation, property_name, line))
		row.add_child(line)
	elif value_type == TYPE_FLOAT:
		var spin := _runtime_spin(float(staged_value))
		spin.custom_minimum_size.x = 150.0
		spin.editable = not mirrored
		spin.value_changed.connect(_on_generation_float_changed.bind(generation, property_name))
		row.add_child(spin)
	else:
		var value_label := LineEdit.new()
		value_label.text = str(staged_value)
		value_label.editable = false
		value_label.custom_minimum_size.x = 150.0
		row.add_child(value_label)

	var live_label := Label.new()
	live_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	if cfg != null and live_names.has(property_name):
		live_label.text = "live: %s" % _short_variant(cfg.get(property_name))
	else:
		live_label.text = "live: —"
	if mirrored:
		live_label.text += "  • canonical elsewhere"
	elif property_name in RUNTIME_GENERATION_FIELDS:
		live_label.text += "  • clipmap"
	else:
		live_label.text += "  • rebuild"
	live_label.modulate = Color(0.55, 0.67, 0.76)
	row.add_child(live_label)


func _on_generation_bool_changed(value: bool, generation: Resource,
		property_name: String) -> void:
	_stage_generation_value(generation, property_name, value)


func _on_generation_int_submitted(text: String, generation: Resource,
		property_name: String, line: LineEdit) -> void:
	if not text.is_valid_int():
		line.text = str(int(generation.get(property_name)))
		_set_status("%s requires an integer." % property_name)
		return
	_stage_generation_value(generation, property_name, text.to_int())


func _on_generation_float_changed(value: float, generation: Resource,
		property_name: String) -> void:
	_stage_generation_value(generation, property_name, value)


func _stage_generation_value(generation: Resource, property_name: String,
		value: Variant) -> void:
	if property_name in MIRRORED_GENERATION_FIELDS:
		_set_status("%s is a mirrored generator value; edit its canonical Planet/Atmospheric control instead." % property_name)
		return
	var scope := WorldAuthoringSession.ApplyScope.CLIPMAP \
		if property_name in RUNTIME_GENERATION_FIELDS \
		else WorldAuthoringSession.ApplyScope.FULL_REBUILD
	_session.stage_set(
		generation,
		StringName(property_name),
		value,
		scope,
		"Change actual generator parameter %s" % property_name)


func _build_runtime_shader_inspector(target_id: String, title: String,
		material: ShaderMaterial) -> void:
	_section(title)
	if material == null:
		_add_note("The production material is not resident yet. It will appear here as soon as that renderer initializes.")
		return
	_remember_default_shader(target_id, material)
	_apply_runtime_shader_state(target_id, material)
	var shader: Shader = material.shader
	if shader == null:
		_add_note("This live ShaderMaterial currently has no Shader assigned.")
		return

	var profile: Resource = _session.active_planet_profile() as Resource
	var explicit_path := String(profile.call("runtime_shader_path", target_id)) \
		if profile != null and profile.has_method("runtime_shader_path") else ""
	var actual_path := shader.resource_path
	var path_row := HBoxContainer.new()
	path_row.add_theme_constant_override("separation", 6)
	_workspace.add_child(path_row)
	var path_label := Label.new()
	path_label.text = "Shader"
	path_label.custom_minimum_size.x = 110.0
	path_row.add_child(path_label)
	var path_edit := LineEdit.new()
	path_edit.text = actual_path
	path_edit.tooltip_text = "Exact Shader resource currently assigned to the live ShaderMaterial"
	path_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	path_edit.text_submitted.connect(_on_runtime_shader_path_submitted.bind(
		target_id, material, path_edit))
	path_row.add_child(path_edit)
	var reset_target := Button.new()
	reset_target.text = "Reset"
	reset_target.tooltip_text = "Restore the production shader and clear all explicit uniform overrides for this target"
	reset_target.disabled = explicit_path.is_empty() and _target_override_count(profile, target_id) == 0
	reset_target.pressed.connect(_reset_runtime_shader_target.bind(target_id, material))
	path_row.add_child(reset_target)

	var dependency_paths := _shader_include_paths(shader)
	if not dependency_paths.is_empty():
		var include_label := Label.new()
		include_label.text = "Includes: %s" % ", ".join(dependency_paths)
		include_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		include_label.modulate = Color(0.52, 0.64, 0.73)
		_workspace.add_child(include_label)

	var uniforms: Array = shader.get_shader_uniform_list()
	var overrides: Dictionary = profile.call("runtime_shader_uniform_overrides", target_id) \
		if profile != null and profile.has_method("runtime_shader_uniform_overrides") else {}
	var expanded := bool(_expanded_shader_targets.get(target_id, false))
	var expand_row := HBoxContainer.new()
	expand_row.add_theme_constant_override("separation", 8)
	_workspace.add_child(expand_row)
	var expand := Button.new()
	expand.text = "Hide uniforms" if expanded else "Show %d actual uniforms" % uniforms.size()
	expand.pressed.connect(func() -> void:
		_expanded_shader_targets[target_id] = not expanded
		_refresh_current_category()
	)
	expand_row.add_child(expand)
	var override_label := Label.new()
	override_label.text = "%d explicit override(s)" % overrides.size()
	override_label.modulate = Color(0.58, 0.70, 0.79)
	expand_row.add_child(override_label)
	if not expanded:
		return

	for uniform_value: Variant in uniforms:
		if not (uniform_value is Dictionary):
			continue
		var uniform: Dictionary = uniform_value as Dictionary
		var uniform_name := String(uniform.get("name", ""))
		if uniform_name.is_empty():
			continue
		_build_runtime_uniform_row(
			target_id,
			material,
			uniform_name,
			material.get_shader_parameter(StringName(uniform_name)),
			overrides.has(uniform_name))


func _build_runtime_uniform_row(target_id: String, material: ShaderMaterial,
		uniform_name: String, value: Variant, overridden: bool) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 4)
	_workspace.add_child(row)
	var label := Label.new()
	label.text = uniform_name
	label.tooltip_text = "Live shader uniform %s" % uniform_name
	label.custom_minimum_size.x = 205.0
	if overridden:
		label.modulate = Color(0.96, 0.72, 0.34)
	row.add_child(label)

	match typeof(value):
		TYPE_BOOL:
			var toggle := CheckButton.new()
			toggle.button_pressed = bool(value)
			toggle.toggled.connect(_on_runtime_bool_changed.bind(
				target_id, material, uniform_name))
			row.add_child(toggle)
		TYPE_INT:
			var spin := _runtime_spin(float(int(value)))
			spin.step = 1.0
			spin.value_changed.connect(_on_runtime_int_changed.bind(
				target_id, material, uniform_name))
			row.add_child(spin)
		TYPE_FLOAT:
			var spin := _runtime_spin(float(value))
			spin.value_changed.connect(_on_runtime_float_changed.bind(
				target_id, material, uniform_name))
			row.add_child(spin)
		TYPE_VECTOR2:
			_build_vector_controls(row, target_id, material, uniform_name, value, 2)
		TYPE_VECTOR3:
			_build_vector_controls(row, target_id, material, uniform_name, value, 3)
		TYPE_VECTOR4:
			_build_vector_controls(row, target_id, material, uniform_name, value, 4)
		TYPE_COLOR:
			var picker := ColorPickerButton.new()
			picker.color = value as Color
			picker.custom_minimum_size = Vector2(145.0, 30.0)
			picker.color_changed.connect(_on_runtime_color_changed.bind(
				target_id, material, uniform_name))
			row.add_child(picker)
		_:
			var text := LineEdit.new()
			text.text = _shader_resource_text(value)
			text.editable = false
			text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			row.add_child(text)

	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(spacer)
	var reset := Button.new()
	reset.text = "↺"
	reset.tooltip_text = "Release this uniform back to the production renderer/shader default"
	reset.disabled = not overridden
	reset.pressed.connect(_clear_runtime_uniform_override.bind(
		target_id, material, uniform_name))
	row.add_child(reset)


func _build_vector_controls(row: HBoxContainer, target_id: String,
		material: ShaderMaterial, uniform_name: String, value: Variant,
		dimension: int) -> void:
	for component: int in dimension:
		var component_value := _vector_component(value, component)
		var spin := _runtime_spin(component_value)
		spin.custom_minimum_size.x = 82.0
		spin.tooltip_text = ["x", "y", "z", "w"][component]
		spin.value_changed.connect(_on_runtime_vector_component_changed.bind(
			target_id, material, uniform_name, component, dimension))
		row.add_child(spin)


func _runtime_spin(value: float) -> SpinBox:
	var spin := SpinBox.new()
	spin.min_value = -1.0e12
	spin.max_value = 1.0e12
	spin.allow_lesser = true
	spin.allow_greater = true
	var magnitude := absf(value)
	spin.step = 0.0001 if magnitude < 0.01 else (0.001 if magnitude < 1.0 else (0.01 if magnitude < 100.0 else 0.1))
	spin.value = value
	spin.custom_minimum_size.x = 145.0
	return spin


func _on_runtime_bool_changed(value: bool, target_id: String,
		material: ShaderMaterial, uniform_name: String) -> void:
	_set_runtime_uniform_override(target_id, material, uniform_name, value)


func _on_runtime_int_changed(value: float, target_id: String,
		material: ShaderMaterial, uniform_name: String) -> void:
	_set_runtime_uniform_override(target_id, material, uniform_name, int(round(value)))


func _on_runtime_float_changed(value: float, target_id: String,
		material: ShaderMaterial, uniform_name: String) -> void:
	_set_runtime_uniform_override(target_id, material, uniform_name, value)


func _on_runtime_color_changed(value: Color, target_id: String,
		material: ShaderMaterial, uniform_name: String) -> void:
	_set_runtime_uniform_override(target_id, material, uniform_name, value)


func _on_runtime_vector_component_changed(component_value: float,
		target_id: String, material: ShaderMaterial, uniform_name: String,
		component: int, dimension: int) -> void:
	var current: Variant = material.get_shader_parameter(StringName(uniform_name))
	var next: Variant = _vector_with_component(current, component, component_value, dimension)
	_set_runtime_uniform_override(target_id, material, uniform_name, next)


func _set_runtime_uniform_override(target_id: String, material: ShaderMaterial,
		uniform_name: String, value: Variant) -> void:
	var profile: Resource = _session.active_planet_profile() as Resource
	if profile == null or not profile.has_method("set_runtime_shader_uniform"):
		return
	material.set_shader_parameter(StringName(uniform_name), value)
	_session.stage_action("Override runtime shader uniform %s" % uniform_name, func() -> void:
		profile.call("set_runtime_shader_uniform", target_id, uniform_name, value)
	, WorldAuthoringSession.ApplyScope.HOT)
	_set_status("Live override: %s.%s = %s" % [target_id, uniform_name, _short_variant(value)])


func _clear_runtime_uniform_override(target_id: String, material: ShaderMaterial,
		uniform_name: String) -> void:
	var profile: Resource = _session.active_planet_profile() as Resource
	if profile == null or not profile.has_method("clear_runtime_shader_uniform"):
		return
	_session.stage_action("Clear runtime shader uniform %s" % uniform_name, func() -> void:
		profile.call("clear_runtime_shader_uniform", target_id, uniform_name)
	, WorldAuthoringSession.ApplyScope.HOT)
	var default_material := ShaderMaterial.new()
	default_material.shader = material.shader
	material.set_shader_parameter(StringName(uniform_name),
		default_material.get_shader_parameter(StringName(uniform_name)))
	_refresh_current_category()


func _on_runtime_shader_path_submitted(path: String, target_id: String,
		material: ShaderMaterial, path_edit: LineEdit) -> void:
	var requested := path.strip_edges()
	if requested.is_empty():
		_reset_runtime_shader_path_only(target_id, material)
		_refresh_current_category()
		return
	if not ResourceLoader.exists(requested, "Shader"):
		path_edit.text = material.shader.resource_path if material.shader != null else ""
		_set_status("Shader does not exist: %s" % requested)
		return
	var loaded: Resource = load(requested)
	if not (loaded is Shader):
		path_edit.text = material.shader.resource_path if material.shader != null else ""
		_set_status("Resource is not a Shader: %s" % requested)
		return
	var profile: Resource = _session.active_planet_profile() as Resource
	if profile == null or not profile.has_method("set_runtime_shader_path"):
		return
	material.shader = loaded as Shader
	_uniform_name_cache.clear()
	_session.stage_action("Change live runtime shader", func() -> void:
		profile.call("set_runtime_shader_path", target_id, requested)
	, WorldAuthoringSession.ApplyScope.HOT)
	_set_status("Live shader changed for %s → %s" % [target_id, requested])
	_refresh_current_category()


func _reset_runtime_shader_target(target_id: String, material: ShaderMaterial) -> void:
	var profile: Resource = _session.active_planet_profile() as Resource
	if profile == null or not profile.has_method("clear_runtime_shader_target"):
		return
	_session.stage_action("Reset live runtime shader target", func() -> void:
		profile.call("clear_runtime_shader_target", target_id)
	, WorldAuthoringSession.ApplyScope.HOT)
	_restore_default_shader(target_id, material)
	_uniform_name_cache.clear()
	_set_status("Restored production shader and released overrides for %s." % target_id)
	_refresh_current_category()


func _reset_runtime_shader_path_only(target_id: String, material: ShaderMaterial) -> void:
	var profile: Resource = _session.active_planet_profile() as Resource
	if profile == null or not profile.has_method("set_runtime_shader_path"):
		return
	_session.stage_action("Restore production runtime shader", func() -> void:
		profile.call("set_runtime_shader_path", target_id, "")
	, WorldAuthoringSession.ApplyScope.HOT)
	_restore_default_shader(target_id, material)
	_uniform_name_cache.clear()


func _remember_default_shader(target_id: String, material: ShaderMaterial) -> void:
	if _default_shader_paths.has(target_id) or material == null or material.shader == null:
		return
	var path := material.shader.resource_path
	if not path.is_empty():
		_default_shader_paths[target_id] = path


func _restore_default_shader(target_id: String, material: ShaderMaterial) -> void:
	if material == null:
		return
	var path := String(_default_shader_paths.get(target_id, ""))
	if path.is_empty() or not ResourceLoader.exists(path, "Shader"):
		return
	var loaded: Resource = load(path)
	if loaded is Shader:
		material.shader = loaded as Shader


func _apply_all_runtime_shader_state() -> void:
	var profile: Resource = _session.active_planet_profile() as Resource if _session != null else null
	if profile == null:
		return
	var materials := _runtime_material_map()
	for target_value: Variant in materials.keys():
		var target_id := String(target_value)
		var material: ShaderMaterial = materials[target_value] as ShaderMaterial
		if material == null:
			continue
		_remember_default_shader(target_id, material)
		_apply_runtime_shader_state(target_id, material)


func _apply_runtime_shader_state(target_id: String, material: ShaderMaterial) -> void:
	var profile: Resource = _session.active_planet_profile() as Resource if _session != null else null
	if profile == null or material == null:
		return
	var explicit_path := String(profile.call("runtime_shader_path", target_id)) \
		if profile.has_method("runtime_shader_path") else ""
	var desired_path := explicit_path
	if desired_path.is_empty():
		desired_path = String(_default_shader_paths.get(target_id, ""))
	var current_path := material.shader.resource_path if material.shader != null else ""
	if not desired_path.is_empty() and desired_path != current_path \
			and ResourceLoader.exists(desired_path, "Shader"):
		var loaded: Resource = load(desired_path)
		if loaded is Shader:
			material.shader = loaded as Shader
	if material.shader == null:
		return
	var valid_names := _shader_uniform_name_set(material.shader)
	var overrides: Dictionary = profile.call("runtime_shader_uniform_overrides", target_id) \
		if profile.has_method("runtime_shader_uniform_overrides") else {}
	for uniform_value: Variant in overrides.keys():
		var uniform_name := String(uniform_value)
		if valid_names.has(uniform_name):
			material.set_shader_parameter(StringName(uniform_name), overrides[uniform_value])


func _runtime_material_map() -> Dictionary:
	var result: Dictionary = {}
	var terrain_material := _runtime_material(TARGET_TERRAIN)
	if terrain_material != null:
		result[TARGET_TERRAIN] = terrain_material
	var ocean_local := _runtime_material(TARGET_OCEAN_LOCAL)
	if ocean_local != null:
		result[TARGET_OCEAN_LOCAL] = ocean_local
	var ocean_orbit := _runtime_material(TARGET_OCEAN_ORBIT)
	if ocean_orbit != null:
		result[TARGET_OCEAN_ORBIT] = ocean_orbit
	var atmosphere := _runtime_material(TARGET_ATMOSPHERE)
	if atmosphere != null:
		result[TARGET_ATMOSPHERE] = atmosphere
	for node: Node in get_tree().get_nodes_in_group(&"authored_water_query"):
		var records_value: Variant = node.get("_records")
		if not (records_value is Array):
			continue
		for record_value: Variant in records_value as Array:
			if not (record_value is Dictionary):
				continue
			var record: Dictionary = record_value as Dictionary
			var feature_id := String(record.get("feature_id", ""))
			var material: ShaderMaterial = record.get("material") as ShaderMaterial
			if not feature_id.is_empty() and material != null:
				result[AUTHORED_WATER_PREFIX + feature_id] = material
	return result


func _runtime_material(target_id: String) -> ShaderMaterial:
	match target_id:
		TARGET_TERRAIN:
			var node := get_node_or_null("/root/GroundGeometryClipmap")
			return node.get("_material") as ShaderMaterial if node != null else null
		TARGET_OCEAN_LOCAL:
			var node := get_node_or_null("/root/OceanSystem")
			return node.get("_material") as ShaderMaterial if node != null else null
		TARGET_OCEAN_ORBIT:
			var orbit: Node = _world_host.get("orbit_ocean") as Node if _world_host != null else null
			if orbit != null and orbit.has_method("material"):
				return orbit.call("material") as ShaderMaterial
			return null
		TARGET_ATMOSPHERE:
			return _world_host.get("sky_mat") as ShaderMaterial if _world_host != null else null
		_:
			return null


func _build_selected_authored_water_shader_inspector() -> void:
	if _selected_water_feature_id.is_empty():
		return
	var target_id := AUTHORED_WATER_PREFIX + _selected_water_feature_id
	var material: ShaderMaterial
	var materials := _runtime_material_map()
	if materials.has(target_id):
		material = materials[target_id] as ShaderMaterial
	_build_runtime_shader_inspector(
		target_id,
		"Selected lake/river — actual render shader",
		material)


func _shader_uniform_name_set(shader: Shader) -> Dictionary:
	if shader == null:
		return {}
	var cache_key := shader.get_instance_id()
	if _uniform_name_cache.has(cache_key):
		return _uniform_name_cache[cache_key] as Dictionary
	var result: Dictionary = {}
	for uniform_value: Variant in shader.get_shader_uniform_list():
		if uniform_value is Dictionary:
			var name := String((uniform_value as Dictionary).get("name", ""))
			if not name.is_empty():
				result[name] = true
	_uniform_name_cache[cache_key] = result
	return result


func _shader_include_paths(shader: Shader) -> PackedStringArray:
	var result := PackedStringArray()
	if shader == null:
		return result
	for source_line: String in shader.code.split("\n"):
		var line := source_line.strip_edges()
		if not line.begins_with("#include"):
			continue
		var first_quote := line.find("\"")
		var last_quote := line.rfind("\"")
		if first_quote >= 0 and last_quote > first_quote:
			result.append(line.substr(first_quote + 1, last_quote - first_quote - 1))
	return result


func _vector_component(value: Variant, component: int) -> float:
	if value is Vector2:
		return (value as Vector2)[component]
	if value is Vector3:
		return (value as Vector3)[component]
	if value is Vector4:
		return (value as Vector4)[component]
	return 0.0


func _vector_with_component(value: Variant, component: int, component_value: float,
		dimension: int) -> Variant:
	if dimension == 2:
		var next := value as Vector2 if value is Vector2 else Vector2.ZERO
		next[component] = component_value
		return next
	if dimension == 3:
		var next := value as Vector3 if value is Vector3 else Vector3.ZERO
		next[component] = component_value
		return next
	var next := value as Vector4 if value is Vector4 else Vector4.ZERO
	next[component] = component_value
	return next


func _shader_resource_text(value: Variant) -> String:
	if value == null:
		return "<unbound>"
	if value is Resource:
		var resource := value as Resource
		return resource.resource_path if not resource.resource_path.is_empty() \
			else "<%s runtime resource>" % resource.get_class()
	return _short_variant(value)


func _short_variant(value: Variant) -> String:
	var text := str(value)
	return text.substr(0, 96) + "…" if text.length() > 96 else text


func _target_override_count(profile: Resource, target_id: String) -> int:
	if profile == null or not profile.has_method("runtime_shader_uniform_overrides"):
		return 0
	return (profile.call("runtime_shader_uniform_overrides", target_id) as Dictionary).size()


func _script_variable_count(resource: Resource) -> int:
	if resource == null:
		return 0
	var count := 0
	for property: Dictionary in resource.get_property_list():
		if (int(property.get("usage", 0)) & PROPERTY_USAGE_SCRIPT_VARIABLE) != 0:
			count += 1
	return count


func _property_name_set(object: Object) -> Dictionary:
	var result: Dictionary = {}
	if object == null:
		return result
	for property: Dictionary in object.get_property_list():
		result[String(property.get("name", ""))] = true
	return result
