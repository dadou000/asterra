class_name WorldAuthoringLiveEditorPhase2
extends "res://scripts/world_authoring/world_authoring_editor_live.gd"
## Transactional live-world terrain authoring.
##
## The base live editor performs immediate Deltas sculpting for zero-latency visual
## feedback. This layer turns runtime edits into proper Planet Studio state: one
## mouse drag becomes one history action, sparse sculpt data round-trips through
## presets, each celestial body owns its own sculpt layer, and Undo/Redo/Revert
## immediately republish the matching Deltas state to rendering and contact.
## Continuous biome painting is batched for the same reason: no per-motion full
## system snapshots or recovery-file writes.

# Kept outside the base enum so this pass can remain an additive editor layer.
# The base placement machinery accepts integer modes, while this subclass owns the
# erase-specific routing/preview/status behavior.
const SCULPT_ERASE_MODE: int = 6

var _sculpt_transaction_active: bool = false
var _sculpt_transaction_body_id: String = ""
var _erase_strength: float = 0.35

var _biome_transaction_active: bool = false
var _biome_transaction_body_id: String = ""
var _biome_transaction_layer_id: String = ""
var _pending_biome_strokes: Array[Dictionary] = []


func _ready() -> void:
	super._ready()
	if _world_host == null:
		return
	_adopt_runtime_sculpt_as_baseline()
	_refresh_toolbar()


func _build_terrain_page() -> void:
	super._build_terrain_page()
	if _world_host == null:
		return
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		return
	_section("Sculpt layer persistence")
	var tile_count: int = int(terrain.call("sculpt_edited_tile_count")) if terrain.has_method("sculpt_edited_tile_count") else 0
	var blob_bytes: int = 0
	if terrain.has_method("sculpt_delta_serialized"):
		var data_value: Variant = terrain.call("sculpt_delta_serialized")
		if data_value is Dictionary:
			var blob_value: Variant = (data_value as Dictionary).get("tiles", PackedByteArray())
			if blob_value is PackedByteArray:
				blob_bytes = (blob_value as PackedByteArray).size()
	var persistence_row := HBoxContainer.new()
	persistence_row.add_theme_constant_override("separation", 7)
	_workspace.add_child(persistence_row)
	var status := Label.new()
	status.text = "%d sparse tile(s) • %.2f MiB packed" % [tile_count, float(blob_bytes) / (1024.0 * 1024.0)]
	status.custom_minimum_size.x = 300.0
	status.modulate = Color(0.64, 0.76, 0.86)
	persistence_row.add_child(status)
	var erase_button := Button.new()
	erase_button.text = "STOP ERASE" if _placement_mode == SCULPT_ERASE_MODE else "ERASE AUTHORED"
	erase_button.tooltip_text = "Brush hand-authored height deltas back toward zero without changing procedural terrain."
	erase_button.pressed.connect(func() -> void:
		_set_placement_mode(PlacementMode.NONE if _placement_mode == SCULPT_ERASE_MODE else SCULPT_ERASE_MODE)
		_refresh_current_category()
	)
	persistence_row.add_child(erase_button)
	var clear_button := Button.new()
	clear_button.text = "CLEAR SCULPT LAYER"
	clear_button.disabled = tile_count <= 0 and Deltas.is_empty()
	clear_button.pressed.connect(_clear_sculpt_layer)
	persistence_row.add_child(clear_button)
	_add_number_field("Erase strength", _erase_strength, 0.01, 1.0, 0.01, "", func(value: float) -> void:
		_erase_strength = value
	)
	_add_note("A complete sculpt drag is one Planet Studio history action. Sculpt deltas are saved with presets and swapped when selecting another celestial body. ERASE AUTHORED reveals the seed-generated surface again and prunes fully empty sparse tiles.")
	_add_note("Viewport shortcuts while sculpting: mouse wheel changes radius, Shift+wheel changes Raise/Lower strength, and holding Shift temporarily inverts Raise/Lower. Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) use Planet Studio history.")


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey:
		var shortcut_key := event as InputEventKey
		if shortcut_key.pressed and not shortcut_key.echo \
				and (shortcut_key.ctrl_pressed or shortcut_key.meta_pressed):
			if shortcut_key.keycode == KEY_Z:
				if shortcut_key.shift_pressed:
					_on_redo_pressed()
				else:
					_on_undo_pressed()
				get_viewport().set_input_as_handled()
				return
			if shortcut_key.keycode == KEY_Y:
				_on_redo_pressed()
				get_viewport().set_input_as_handled()
				return

	if event is InputEventMouseButton:
		var wheel_event := event as InputEventMouseButton
		if wheel_event.pressed and _is_sculpt_placement() \
				and _is_live_viewport_point(wheel_event.position) \
				and (wheel_event.button_index == MOUSE_BUTTON_WHEEL_UP \
				or wheel_event.button_index == MOUSE_BUTTON_WHEEL_DOWN):
			var direction_scale: float = 1.12 if wheel_event.button_index == MOUSE_BUTTON_WHEEL_UP else 1.0 / 1.12
			if wheel_event.shift_pressed and _placement_mode != SCULPT_ERASE_MODE:
				_sculpt_strength_m = clampf(_sculpt_strength_m * direction_scale, 0.01, 20.0)
				_set_status("Sculpt strength %.3f m/stamp." % _sculpt_strength_m)
			else:
				_sculpt_radius_m = clampf(_sculpt_radius_m * direction_scale, 0.5, 150.0)
				_set_status("Sculpt radius %.2f m." % _sculpt_radius_m)
			_update_preview()
			get_viewport().set_input_as_handled()
			return

	var should_finish_transactions := false
	if event is InputEventMouseButton:
		var mouse_button := event as InputEventMouseButton
		if mouse_button.button_index == MOUSE_BUTTON_LEFT:
			if mouse_button.pressed and _is_live_viewport_point(mouse_button.position):
				if _is_sculpt_placement():
					_begin_sculpt_transaction()
				elif _placement_mode == PlacementMode.BIOME:
					_begin_biome_transaction()
			elif not mouse_button.pressed \
					and (_sculpt_transaction_active or _biome_transaction_active):
				should_finish_transactions = true
		elif mouse_button.button_index == MOUSE_BUTTON_RIGHT and mouse_button.pressed \
				and (_sculpt_transaction_active or _biome_transaction_active):
			should_finish_transactions = true
	elif event is InputEventKey:
		var key_event := event as InputEventKey
		if key_event.pressed and not key_event.echo and key_event.keycode == KEY_ESCAPE \
				and (_sculpt_transaction_active or _biome_transaction_active):
			should_finish_transactions = true

	super._unhandled_input(event)

	if should_finish_transactions:
		_commit_interactive_transactions()


func _is_sculpt_placement() -> bool:
	return _placement_mode == PlacementMode.SCULPT_RAISE \
		or _placement_mode == PlacementMode.SCULPT_LOWER \
		or _placement_mode == SCULPT_ERASE_MODE


func _continuous_drag_mode() -> bool:
	return super._continuous_drag_mode() or _placement_mode == SCULPT_ERASE_MODE


func _placement_status_text() -> String:
	if _placement_mode == SCULPT_ERASE_MODE:
		return "SCULPT ERASE — LMB restores generated terrain • wheel radius • RMB/Esc stop"
	if _is_sculpt_placement():
		var base: String = super._placement_status_text()
		return "%s • Shift invert • wheel radius • Shift+wheel strength" % base
	return super._placement_status_text()


func _place_current_hit(continuous: bool) -> void:
	if _placement_mode != SCULPT_ERASE_MODE:
		super._place_current_hit(continuous)
		return
	if _last_hit.is_empty():
		_set_status("Viewport pick did not intersect terrain.")
		return
	var direction: Vector3 = _last_hit.get("dir", Vector3.ZERO)
	if direction.length_squared() < 0.99:
		return
	_place_erase_stroke(direction, continuous)
	_update_preview()


func _place_sculpt_stroke(direction: Vector3, continuous: bool, sign_value: float) -> void:
	var effective_sign: float = -sign_value if Input.is_key_pressed(KEY_SHIFT) else sign_value
	super._place_sculpt_stroke(direction, continuous, effective_sign)


func _place_erase_stroke(direction: Vector3, continuous: bool) -> void:
	if Planet.cfg == null:
		return
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if continuous and _last_sculpt_dir.length_squared() > 0.99:
		var arc_distance: float = acos(clampf(_last_sculpt_dir.dot(direction), -1.0, 1.0)) * planet_radius
		if arc_distance < maxf(_sculpt_radius_m * 0.16, 0.2):
			return
	var changed: int = int(Deltas.erase_radial_brush(
		direction,
		_sculpt_radius_m,
		_erase_strength,
		_sculpt_hardness,
		planet_radius
	))
	if changed <= 0:
		return
	_last_sculpt_dir = direction
	_set_status("Erased authored terrain: %d samples • %.1f m radius • %.0f%% strength." % [changed, _sculpt_radius_m, _erase_strength * 100.0])


func _update_preview() -> void:
	if _placement_mode != SCULPT_ERASE_MODE:
		super._update_preview()
		return
	if _preview_mesh == null:
		return
	_preview_mesh.clear_surfaces()
	if _navigation_active:
		return
	if not _last_hit.is_empty():
		var direction: Vector3 = _last_hit.get("dir", Vector3.ZERO)
		var height: float = float(_last_hit.get("height", 0.0))
		_draw_surface_ring(direction, height, _sculpt_radius_m, Color(0.92, 0.92, 0.98, 1.0))
		if _sculpt_hardness > 0.02:
			_draw_surface_ring(direction, height, maxf(0.1, _sculpt_radius_m * _sculpt_hardness), Color(0.62, 0.66, 0.74, 0.85))
	_draw_selected_water_feature()


func _place_biome_stroke(direction: Vector3, continuous: bool) -> void:
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		return
	var layer: Resource = terrain.call("find_biome_layer", _selected_biome_layer_id) as Resource
	if layer == null:
		_set_status("Select a biome paint layer before painting.")
		return
	if not _biome_transaction_active:
		_begin_biome_transaction()
	var radius_m: float = float(layer.get(&"brush_radius_m"))
	if continuous and _last_paint_dir.length_squared() > 0.99:
		var body: Resource = _session.active_body() as Resource
		var planet_radius: float = maxf(float(body.get(&"radius_m")), 1.0) if body != null else maxf(float(Planet.cfg.planet_radius), 1.0)
		var arc_distance: float = acos(clampf(_last_paint_dir.dot(direction), -1.0, 1.0)) * planet_radius
		if arc_distance < maxf(radius_m * 0.22, 0.25):
			return
	var biome_id: int = int(layer.get(&"active_biome_id"))
	var hardness: float = float(layer.get(&"brush_hardness"))
	var opacity: float = float(layer.get(&"brush_opacity"))
	_pending_biome_strokes.append({
		"dir": direction.normalized(),
		"biome": biome_id,
		"radius": radius_m,
		"hardness": hardness,
		"opacity": opacity,
	})
	_last_paint_dir = direction
	_set_status("Painting %s • %d pending stamp(s) • %.1f m radius." % [BIOME_NAMES[biome_id], _pending_biome_strokes.size(), radius_m])


func _begin_sculpt_transaction() -> void:
	if _sculpt_transaction_active:
		return
	_sculpt_transaction_active = true
	_sculpt_transaction_body_id = _active_body_id()


func _begin_biome_transaction() -> void:
	if _biome_transaction_active:
		return
	_biome_transaction_active = true
	_biome_transaction_body_id = _active_body_id()
	_biome_transaction_layer_id = _selected_biome_layer_id
	_pending_biome_strokes.clear()


func _commit_interactive_transactions() -> void:
	_commit_sculpt_transaction()
	_commit_biome_transaction()


func _commit_sculpt_transaction() -> void:
	if not _sculpt_transaction_active:
		return
	_sculpt_transaction_active = false
	var started_body_id: String = _sculpt_transaction_body_id
	_sculpt_transaction_body_id = ""
	if started_body_id.is_empty() or started_body_id != _active_body_id():
		_sync_runtime_sculpt_from_profile()
		return
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null or not terrain.has_method("set_sculpt_delta_serialized"):
		return
	var snapshot: Dictionary = Deltas.serialize()
	if terrain.has_method("sculpt_delta_matches") and bool(terrain.call("sculpt_delta_matches", snapshot)):
		return
	_session.stage_action("Sculpt terrain", func() -> void:
		terrain.call("set_sculpt_delta_serialized", snapshot)
	, SESSION_SCRIPT.ApplyScope.TILES)
	_refresh_toolbar()


func _commit_biome_transaction() -> void:
	if not _biome_transaction_active:
		return
	_biome_transaction_active = false
	var started_body_id: String = _biome_transaction_body_id
	var started_layer_id: String = _biome_transaction_layer_id
	_biome_transaction_body_id = ""
	_biome_transaction_layer_id = ""
	if _pending_biome_strokes.is_empty():
		return
	if started_body_id != _active_body_id() or started_layer_id != _selected_biome_layer_id:
		_pending_biome_strokes.clear()
		return
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null:
		_pending_biome_strokes.clear()
		return
	var layer: Resource = terrain.call("find_biome_layer", started_layer_id) as Resource
	if layer == null:
		_pending_biome_strokes.clear()
		return
	var strokes: Array[Dictionary] = _pending_biome_strokes.duplicate(true)
	_pending_biome_strokes.clear()
	_session.stage_action("Paint biome stroke", func() -> void:
		for stroke: Dictionary in strokes:
			layer.call(
				"add_stroke",
				stroke["dir"],
				int(stroke["biome"]),
				float(stroke["radius"]),
				float(stroke["hardness"]),
				float(stroke["opacity"])
			)
	, SESSION_SCRIPT.ApplyScope.TILES)
	_set_status("Committed %d biome paint stamp(s) as one history action." % strokes.size())
	_refresh_toolbar()


func _discard_interactive_transactions() -> void:
	_sculpt_transaction_active = false
	_sculpt_transaction_body_id = ""
	_biome_transaction_active = false
	_biome_transaction_body_id = ""
	_biome_transaction_layer_id = ""
	_pending_biome_strokes.clear()


func _clear_sculpt_layer() -> void:
	_commit_interactive_transactions()
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null or not terrain.has_method("clear_sculpt_deltas"):
		return
	var had_profile_data: bool = bool(terrain.call("has_sculpt_deltas")) if terrain.has_method("has_sculpt_deltas") else false
	if not had_profile_data and Deltas.is_empty():
		return
	_session.stage_action("Clear terrain sculpt", func() -> void:
		terrain.call("clear_sculpt_deltas")
	, SESSION_SCRIPT.ApplyScope.TILES)
	Deltas.clear()
	_last_sculpt_dir = Vector3.ZERO
	_last_hit.clear()
	_update_preview()
	_refresh_current_category()
	_set_status("Cleared the active body's sparse sculpt layer. Undo restores it immediately.")


func _on_undo_pressed() -> void:
	_commit_interactive_transactions()
	super._on_undo_pressed()
	_sync_runtime_sculpt_from_profile()


func _on_redo_pressed() -> void:
	_commit_interactive_transactions()
	super._on_redo_pressed()
	_sync_runtime_sculpt_from_profile()


func _on_apply_pressed() -> void:
	_commit_interactive_transactions()
	super._on_apply_pressed()
	_sync_runtime_sculpt_from_profile()


func _on_revert_pressed() -> void:
	_discard_interactive_transactions()
	super._on_revert_pressed()
	_sync_runtime_sculpt_from_profile()


func _on_body_selected(index: int) -> void:
	_commit_interactive_transactions()
	super._on_body_selected(index)
	_sync_runtime_sculpt_from_profile()
	_last_hit.clear()
	_last_sculpt_dir = Vector3.ZERO
	_update_preview()


func _on_preset_selected(path: String) -> void:
	_commit_interactive_transactions()
	super._on_preset_selected(path)
	_sync_runtime_sculpt_from_profile()
	_last_hit.clear()
	_last_sculpt_dir = Vector3.ZERO
	_update_preview()


func _active_body_id() -> String:
	if _session == null or _session.staged_system == null:
		return ""
	return String(_session.staged_system.get(&"active_body_id"))


func _adopt_runtime_sculpt_as_baseline() -> void:
	if _session == null:
		return
	var snapshot: Dictionary = Deltas.serialize()
	var staged_terrain: Resource = _session.active_terrain_profile() as Resource
	if staged_terrain != null and staged_terrain.has_method("set_sculpt_delta_serialized"):
		staged_terrain.call("set_sculpt_delta_serialized", snapshot)
	var applied_terrain: Resource = _active_terrain_for_system(_session.applied_system)
	if applied_terrain != null and applied_terrain.has_method("set_sculpt_delta_serialized"):
		applied_terrain.call("set_sculpt_delta_serialized", snapshot)


func _active_terrain_for_system(system: Resource) -> Resource:
	if system == null or not system.has_method("active_body"):
		return null
	var body: Resource = system.call("active_body") as Resource
	if body == null:
		return null
	var profile: Resource = body.get(&"planet_profile") as Resource
	if profile == null:
		return null
	return profile.get(&"terrain") as Resource


func _sync_runtime_sculpt_from_profile() -> void:
	var terrain: Resource = _session.active_terrain_profile() as Resource
	if terrain == null or not terrain.has_method("sculpt_delta_serialized"):
		Deltas.clear()
		return
	var data_value: Variant = terrain.call("sculpt_delta_serialized")
	if not (data_value is Dictionary):
		Deltas.clear()
		return
	Deltas.deserialize(data_value as Dictionary)
	_last_hit.clear()
	_last_sculpt_dir = Vector3.ZERO
	_update_preview()
