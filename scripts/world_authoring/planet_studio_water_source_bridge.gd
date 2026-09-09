extends Node
## Planet Studio integration for the production sparse-hydrology point-source API.
##
## This deliberately sits beside the editor rather than duplicating hydrology.
## It injects a compact authoring panel into the existing WATER page, reuses the
## live editor's authoritative terrain picker, and mirrors staged source resources
## into WaterSystem. The solver, sparse allocation, reconstruction cache and water
## renderer remain the production systems used by normal gameplay.

const PANEL_NAME := "SparseWaterSourceAuthoringPanel"
const LIVE_EDITOR_NAME := "PlanetStudioLive"
const LIVE_VIEW_MIN_X := 1082.0
const MARKER_SEGMENTS := 40
const MARKER_ALTITUDE_M := 0.18

var _editor: Control
var _session: WorldAuthoringSession
var _workspace: VBoxContainer
var _selected_source_id := ""
var _placing_source_id := ""
var _runtime_source_signatures: Dictionary = {} # source_id -> String
var _panel_rebuild_requested := false
var _marker_mesh: ImmediateMesh
var _marker_instance: MeshInstance3D
var _marker_material: StandardMaterial3D
var _last_marker_update_usec := 0


func _ready() -> void:
	process_priority = 205
	set_process(true)
	set_process_unhandled_input(true)


func _process(_delta: float) -> void:
	if not _planet_studio_running():
		_hide_markers()
		return
	if _editor == null or not is_instance_valid(_editor):
		_try_bind_editor()
		return
	_workspace = _editor.get("_workspace") as VBoxContainer
	if _workspace == null or not is_instance_valid(_workspace):
		return
	if String(_editor.get("_category")) == "WATER":
		_ensure_panel()
	else:
		_hide_markers()
		return
	var now := Time.get_ticks_usec()
	if now - _last_marker_update_usec >= 100000:
		_last_marker_update_usec = now
		_redraw_source_markers()


func _unhandled_input(event: InputEvent) -> void:
	if _placing_source_id.is_empty() or _editor == null or not is_instance_valid(_editor):
		return
	if event is InputEventKey:
		var key := event as InputEventKey
		if key.pressed and key.keycode == KEY_ESCAPE:
			_cancel_placement("Water source placement cancelled.")
			get_viewport().set_input_as_handled()
			return
	if not (event is InputEventMouseButton):
		return
	var mouse := event as InputEventMouseButton
	if not mouse.pressed:
		return
	if mouse.button_index == MOUSE_BUTTON_RIGHT:
		_cancel_placement("Water source placement cancelled.")
		get_viewport().set_input_as_handled()
		return
	if mouse.button_index != MOUSE_BUTTON_LEFT or mouse.position.x <= LIVE_VIEW_MIN_X:
		return
	var hit_value: Variant = _editor.call("_screen_aim", mouse.position)
	if not (hit_value is Dictionary):
		return
	var hit := hit_value as Dictionary
	var direction := Vector3(hit.get("dir", Vector3.ZERO))
	if direction.length_squared() < 0.99:
		_set_editor_status("Water source placement did not intersect the detailed terrain.")
		return
	_place_selected_source(direction.normalized())
	get_viewport().set_input_as_handled()


func _planet_studio_running() -> bool:
	return get_tree().has_meta("launch_mode") \
		and String(get_tree().get_meta("launch_mode")) == "planet_studio"


func _try_bind_editor() -> void:
	var candidate := get_tree().root.find_child(LIVE_EDITOR_NAME, true, false) as Control
	if candidate == null:
		return
	var session_value: Variant = candidate.get("_session")
	if not (session_value is WorldAuthoringSession):
		return
	_editor = candidate
	_session = session_value as WorldAuthoringSession
	_workspace = _editor.get("_workspace") as VBoxContainer
	if not _session.changed.is_connected(_on_session_changed):
		_session.changed.connect(_on_session_changed)
	_create_marker_renderer()
	_sync_sources_to_runtime()


func _on_session_changed(_dirty: bool, _scope: int) -> void:
	_sync_sources_to_runtime()
	_panel_rebuild_requested = true


func _ensure_panel() -> void:
	if _workspace == null:
		return
	var existing := _workspace.get_node_or_null(PANEL_NAME)
	if existing != null and not _panel_rebuild_requested:
		return
	_panel_rebuild_requested = false
	if existing != null:
		_workspace.remove_child(existing)
		existing.queue_free()
	_build_panel()


func _build_panel() -> void:
	var water := _water_profile()
	if water == null or _workspace == null:
		return
	var panel := PanelContainer.new()
	panel.name = PANEL_NAME
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_workspace.add_child(panel)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 7)
	panel.add_child(box)

	var title := Label.new()
	title.text = "SPARSE HYDROLOGY TEST SOURCES"
	title.add_theme_font_size_override("font_size", 17)
	box.add_child(title)
	var note := Label.new()
	note.text = "Place real volumetric sources into the production sparse SWE solver. Positive flow creates water; negative flow acts as a sink and does not create a dry domain."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.modulate = Color(0.61, 0.72, 0.81)
	box.add_child(note)

	var preview := CheckButton.new()
	preview.text = "Show simulated inland / flood water"
	preview.button_pressed = _hydrology_preview_enabled()
	preview.toggled.connect(_set_hydrology_preview_enabled)
	box.add_child(preview)

	var runtime := Label.new()
	runtime.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	runtime.modulate = Color(0.55, 0.68, 0.77)
	runtime.text = _runtime_status_text()
	box.add_child(runtime)

	var actions := HBoxContainer.new()
	actions.add_theme_constant_override("separation", 7)
	box.add_child(actions)
	var add := Button.new()
	add.text = "+ Add Source"
	add.pressed.connect(_create_source)
	actions.add_child(add)
	var resync := Button.new()
	resync.text = "Resync Runtime"
	resync.pressed.connect(func() -> void:
		_runtime_source_signatures.clear()
		_sync_sources_to_runtime()
		_set_editor_status("Water sources resynchronized with the sparse runtime.")
	)
	actions.add_child(resync)

	var sources: Array = water.get(&"point_sources")
	if sources.is_empty():
		var empty := Label.new()
		empty.text = "No test sources. Add one, then click Place on Planet."
		empty.modulate = Color(0.58, 0.67, 0.74)
		box.add_child(empty)
		return

	var list := ItemList.new()
	list.custom_minimum_size.y = minf(180.0, 44.0 + float(sources.size()) * 30.0)
	for source_value: Variant in sources:
		var source := source_value as Resource
		if source == null:
			continue
		var id := String(source.get(&"source_id"))
		var state := "ON" if bool(source.get(&"enabled")) else "OFF"
		var index := list.add_item("%s   ·   %.2f m³/s   ·   %s" % [
			String(source.get(&"display_name")), float(source.get(&"rate_m3_s")), state])
		list.set_item_metadata(index, id)
		if id == _selected_source_id:
			list.select(index)
	if _selected_source_id.is_empty() and list.item_count > 0:
		_selected_source_id = String(list.get_item_metadata(0))
		list.select(0)
	list.item_selected.connect(func(index: int) -> void:
		_selected_source_id = String(list.get_item_metadata(index))
		_panel_rebuild_requested = true
	)
	box.add_child(list)

	var source := water.call("find_point_source", _selected_source_id) as Resource
	if source == null:
		return
	var separator := HSeparator.new()
	box.add_child(separator)
	_build_selected_source_controls(box, source)


func _build_selected_source_controls(box: VBoxContainer, source: Resource) -> void:
	var name_edit := LineEdit.new()
	name_edit.text = String(source.get(&"display_name"))
	name_edit.placeholder_text = "Source name"
	name_edit.text_submitted.connect(func(value: String) -> void:
		_stage_source_edit(source, "Rename water source", func() -> void:
			source.set(&"display_name", value.strip_edges() if not value.strip_edges().is_empty() else "Water Source")
		)
	)
	box.add_child(name_edit)

	var enabled := CheckButton.new()
	enabled.text = "Source enabled"
	enabled.button_pressed = bool(source.get(&"enabled"))
	enabled.toggled.connect(func(value: bool) -> void:
		_stage_source_edit(source, "Toggle water source", func() -> void: source.set(&"enabled", value))
	)
	box.add_child(enabled)

	var flow_row := HBoxContainer.new()
	flow_row.add_theme_constant_override("separation", 8)
	box.add_child(flow_row)
	var flow_label := Label.new()
	flow_label.text = "Volumetric flow"
	flow_label.custom_minimum_size.x = 150.0
	flow_row.add_child(flow_label)
	var flow := SpinBox.new()
	flow.min_value = -1000000.0
	flow.max_value = 1000000.0
	flow.step = 0.1
	flow.suffix = " m³/s"
	flow.value = float(source.get(&"rate_m3_s"))
	flow.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	flow.value_changed.connect(func(value: float) -> void:
		_stage_source_edit(source, "Change water source flow", func() -> void: source.set(&"rate_m3_s", value), false)
	)
	flow_row.add_child(flow)

	var direction := Vector3(source.get(&"direction", Vector3.UP)).normalized()
	var lat := rad_to_deg(asin(clampf(direction.y, -1.0, 1.0)))
	var lon := rad_to_deg(atan2(direction.x, direction.z))
	var location := Label.new()
	location.text = "Location: %.5f° lat, %.5f° lon   ·   tile level: %s" % [
		lat, lon, "auto" if int(source.get(&"tile_level")) < 0 else str(int(source.get(&"tile_level")))]
	location.modulate = Color(0.60, 0.72, 0.81)
	box.add_child(location)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 7)
	box.add_child(row)
	var place := Button.new()
	place.text = "CLICK TERRAIN TO PLACE" if _placing_source_id == String(source.get(&"source_id")) else "Place on Planet"
	place.pressed.connect(func() -> void:
		_placing_source_id = "" if _placing_source_id == String(source.get(&"source_id")) \
			else String(source.get(&"source_id"))
		_set_editor_status("WATER SOURCE ARMED — click detailed terrain • RMB/Esc cancel" if not _placing_source_id.is_empty() \
			else "Water source placement stopped.")
		_panel_rebuild_requested = true
	)
	row.add_child(place)
	var focus := Button.new()
	focus.text = "Center Water Cache"
	focus.pressed.connect(func() -> void:
		_focus_water_cache(Vector3(source.get(&"direction", Vector3.UP)))
	)
	row.add_child(focus)
	var remove := Button.new()
	remove.text = "Delete"
	remove.pressed.connect(func() -> void: _delete_source(String(source.get(&"source_id"))))
	row.add_child(remove)

	var advanced := Label.new()
	advanced.text = "Injection velocity is currently zero, so water enters without artificial tangential momentum. The runtime chooses the metric-compatible sparse tile level automatically."
	advanced.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	advanced.modulate = Color(0.52, 0.63, 0.71)
	box.add_child(advanced)


func _create_source() -> void:
	var water := _water_profile()
	if water == null or _session == null:
		return
	var created_box: Array[Resource] = []
	var next_number := (water.get(&"point_sources") as Array).size() + 1
	_session.stage_action("Create water source", func() -> void:
		var created := water.call("create_point_source", "Source %d" % next_number) as Resource
		if created != null:
			created.set(&"enabled", false)
			created.set(&"rate_m3_s", 10.0)
			created_box.append(created)
	, WorldAuthoringSession.ApplyScope.TILES)
	if created_box.is_empty():
		return
	_selected_source_id = String(created_box[0].get(&"source_id"))
	_placing_source_id = _selected_source_id
	_set_hydrology_preview_enabled(true)
	_set_editor_status("New water source created. Click the detailed Asterra terrain to place and enable it.")
	_panel_rebuild_requested = true


func _delete_source(source_id: String) -> void:
	var water := _water_profile()
	if water == null or source_id.is_empty():
		return
	_session.stage_action("Delete water source", func() -> void:
		water.call("remove_point_source", source_id)
	, WorldAuthoringSession.ApplyScope.TILES)
	if _placing_source_id == source_id:
		_placing_source_id = ""
	if _selected_source_id == source_id:
		_selected_source_id = ""
	_panel_rebuild_requested = true


func _stage_source_edit(source: Resource, action_name: String, edit: Callable,
		rebuild_panel: bool = true) -> void:
	if source == null or _session == null or not edit.is_valid():
		return
	_session.stage_action(action_name, edit, WorldAuthoringSession.ApplyScope.TILES)
	if rebuild_panel:
		_panel_rebuild_requested = true


func _place_selected_source(direction: Vector3) -> void:
	var water := _water_profile()
	if water == null:
		return
	var source := water.call("find_point_source", _placing_source_id) as Resource
	if source == null:
		_cancel_placement("The selected water source no longer exists.")
		return
	_session.stage_action("Place water source on planet", func() -> void:
		source.set(&"direction", direction)
		source.set(&"enabled", true)
	, WorldAuthoringSession.ApplyScope.TILES)
	_selected_source_id = String(source.get(&"source_id"))
	_placing_source_id = ""
	_set_hydrology_preview_enabled(true)
	_focus_water_cache(direction)
	_sync_sources_to_runtime()
	_set_editor_status("Placed %s at %.2f m³/s. Sparse hydrology is running; water should accumulate and follow the terrain." % [
		String(source.get(&"display_name")), float(source.get(&"rate_m3_s"))])
	_panel_rebuild_requested = true
	_redraw_source_markers()


func _cancel_placement(message: String) -> void:
	_placing_source_id = ""
	_set_editor_status(message)
	_panel_rebuild_requested = true


func _water_profile() -> Resource:
	if _session == null:
		return null
	return _session.active_water_profile() as Resource


func _runtime_target_available() -> bool:
	if _session == null or not Planet.ready_state or Planet.cfg == null:
		return false
	var body := _session.active_body() as Resource
	if body == null:
		return false
	var runtime_radius := maxf(float(Planet.cfg.planet_radius), 1.0)
	return absf(float(body.get(&"radius_m")) - runtime_radius) \
		<= maxf(2.0, runtime_radius * 1.0e-5)


func _source_signature(source: Resource) -> String:
	var d := Vector3(source.get(&"direction", Vector3.UP)).normalized()
	var v := Vector3(source.get(&"injection_velocity_world", Vector3.ZERO))
	return "%.10f|%.10f|%.10f|%.10f|%.10f|%.10f|%.10f|%d|%d" % [
		d.x, d.y, d.z, float(source.get(&"rate_m3_s")), v.x, v.y, v.z,
		int(source.get(&"tile_level")), 1 if bool(source.get(&"enabled")) else 0]


func _sync_sources_to_runtime() -> void:
	if not _runtime_target_available():
		return
	var water := _water_profile()
	if water == null:
		return
	var seen: Dictionary = {}
	var sources: Array = water.get(&"point_sources")
	for source_value: Variant in sources:
		var source := source_value as Resource
		if source == null:
			continue
		source.call("ensure_valid")
		var source_id := String(source.get(&"source_id"))
		if source_id.is_empty():
			continue
		seen[source_id] = true
		var signature := _source_signature(source)
		if String(_runtime_source_signatures.get(source_id, "")) == signature:
			continue
		var err := WaterSystem.upsert_point_water_source(
			source_id,
			Vector3(source.get(&"direction", Vector3.UP)),
			float(source.get(&"rate_m3_s")),
			Vector3(source.get(&"injection_velocity_world", Vector3.ZERO)),
			int(source.get(&"tile_level")),
			bool(source.get(&"enabled")))
		if err == OK:
			_runtime_source_signatures[source_id] = signature
		else:
			_set_editor_status("Water source runtime sync failed for %s (error %d)." % [source_id, int(err)])
	var stale: Array[String] = []
	for id_value: Variant in _runtime_source_signatures.keys():
		var id := String(id_value)
		if not seen.has(id):
			stale.append(id)
	for id: String in stale:
		WaterSystem.remove_point_water_source(id)
		_runtime_source_signatures.erase(id)


func _set_hydrology_preview_enabled(enabled: bool) -> void:
	WaterSystem.set_dynamic_surface_render_enabled(enabled)
	LocalWaterSurface.set_render_enabled(enabled)
	if enabled and SparseHydroSurfaceCache.available():
		SparseHydroSurfaceCache.request_update()
	_panel_rebuild_requested = true


func _hydrology_preview_enabled() -> bool:
	return WaterSystem.dynamic_surface_render_enabled() and LocalWaterSurface.is_render_enabled()


func _focus_water_cache(direction: Vector3) -> void:
	if direction.length_squared() < 0.5:
		return
	WaterSystem.set_dynamic_surface_anchor_direction(direction.normalized())
	WaterSystem.set_dynamic_surface_center_plane(Vector2.ZERO)
	if SparseHydroSurfaceCache.available():
		SparseHydroSurfaceCache.request_update()


func _runtime_status_text() -> String:
	return "Sparse runtime: %s   ·   sources: %d   ·   reconstruction: %s   ·   local renderer: %s" % [
		WaterSystem.sparse_runtime_state(),
		WaterSystem.point_water_source_count(),
		"READY" if SparseHydroSurfaceCache.available() else "WAITING",
		"READY" if LocalWaterSurface.renderer_available() else "WAITING"]


func _set_editor_status(message: String) -> void:
	if _editor != null and is_instance_valid(_editor):
		_editor.call("_set_status", message)
		var live_label := _editor.get("_live_status_label") as Label
		if live_label != null:
			live_label.text = message


func _create_marker_renderer() -> void:
	if _marker_instance != null and is_instance_valid(_marker_instance):
		return
	if _editor == null:
		return
	var world_host := _editor.get("_world_host") as Node
	if world_host == null or not (world_host is Node3D):
		return
	_marker_mesh = ImmediateMesh.new()
	_marker_instance = MeshInstance3D.new()
	_marker_instance.name = "PlanetStudioWaterSourceMarkers"
	_marker_instance.mesh = _marker_mesh
	_marker_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_marker_instance.extra_cull_margin = 1000000.0
	_marker_material = StandardMaterial3D.new()
	_marker_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_marker_material.vertex_color_use_as_albedo = true
	_marker_material.no_depth_test = true
	_marker_instance.material_override = _marker_material
	(world_host as Node3D).add_child(_marker_instance)


func _redraw_source_markers() -> void:
	if _marker_mesh == null or Planet.cfg == null or not Planet.ready_state:
		return
	_marker_mesh.clear_surfaces()
	var water := _water_profile()
	if water == null:
		return
	var sources: Array = water.get(&"point_sources")
	for source_value: Variant in sources:
		var source := source_value as Resource
		if source == null:
			continue
		var direction := Vector3(source.get(&"direction", Vector3.UP)).normalized()
		if direction.length_squared() < 0.99:
			continue
		var source_id := String(source.get(&"source_id"))
		var selected := source_id == _selected_source_id
		var enabled := bool(source.get(&"enabled"))
		var color := Color(0.22, 0.94, 1.0, 1.0) if enabled else Color(0.42, 0.52, 0.58, 1.0)
		if selected:
			color = Color(1.0, 0.78, 0.18, 1.0)
		var magnitude := absf(float(source.get(&"rate_m3_s")))
		var radius_m := clampf(2.5 + pow(magnitude + 1.0, 0.25), 3.0, 14.0)
		_draw_marker_ring(direction, radius_m, color)


func _draw_marker_ring(direction: Vector3, radius_m: float, color: Color) -> void:
	var up := direction.normalized()
	var coarse := TerrainContactSampler.coarse_height(up)
	var height := TerrainContactSampler.contact_height(up, coarse)
	var reference := Vector3.UP if absf(up.dot(Vector3.UP)) < 0.95 else Vector3.RIGHT
	var tangent_x := reference.cross(up).normalized()
	var tangent_y := up.cross(tangent_x).normalized()
	var base_radius := float(Planet.cfg.planet_radius) + height + MARKER_ALTITUDE_M
	_marker_mesh.surface_begin(Mesh.PRIMITIVE_LINE_STRIP)
	for index: int in MARKER_SEGMENTS + 1:
		var angle := TAU * float(index) / float(MARKER_SEGMENTS)
		var offset := tangent_x * cos(angle) * radius_m + tangent_y * sin(angle) * radius_m
		var world_point := Vec3D.from_v3(up).mul(base_radius).add(Vec3D.from_v3(offset))
		_marker_mesh.surface_set_color(color)
		_marker_mesh.surface_add_vertex(Frames.to_render(world_point))
	_marker_mesh.surface_end()
	# Cross-hair makes a point emitter readable even when the ring is viewed edge-on.
	_marker_mesh.surface_begin(Mesh.PRIMITIVE_LINES)
	for axis: Vector3 in [tangent_x, tangent_y]:
		for sign_value: float in [-1.0, 1.0]:
			var edge := Vec3D.from_v3(up).mul(base_radius).add(
				Vec3D.from_v3(axis * radius_m * sign_value))
			_marker_mesh.surface_set_color(color)
			_marker_mesh.surface_add_vertex(Frames.to_render(Vec3D.from_v3(up).mul(base_radius)))
			_marker_mesh.surface_set_color(color)
			_marker_mesh.surface_add_vertex(Frames.to_render(edge))
	_marker_mesh.surface_end()


func _hide_markers() -> void:
	if _marker_mesh != null:
		_marker_mesh.clear_surfaces()
