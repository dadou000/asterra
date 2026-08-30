extends "res://scripts/world_authoring/world_authoring_editor_live_phase21.gd"
## Phase 22: selected celestial bodies drive the live 3D preview.
##
## The runtime host also observes staged active_body_id changes, so list/map body
## selection and newly created bodies focus automatically. This signal provides an
## explicit re-focus path for the toolbar selector and the Focus button even when
## the already-active body is selected again.

signal body_focus_requested(body_id: String)


func _on_body_selected(index: int) -> void:
	if index < 0 or index >= _body_selector.item_count:
		return
	var body_id: String = String(_body_selector.get_item_metadata(index))
	_session.select_body(body_id)
	body_focus_requested.emit(body_id)
	_refresh_all()


func _build_celestials_page() -> void:
	super._build_celestials_page()
	_section("3D preview")
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	_workspace.add_child(row)
	var focus := _toolbar_button("Focus Selected")
	focus.tooltip_text = "Frame the currently selected planet, moon or star in the live 3D viewport."
	focus.pressed.connect(_request_active_body_focus)
	row.add_child(focus)
	var body: Resource = _session.active_body()
	var name_text: String = String(body.get(&"display_name")) if body != null else "none"
	var note := Label.new()
	note.text = "Viewport target: %s" % name_text
	note.modulate = Color(0.58, 0.70, 0.79)
	row.add_child(note)
	_add_note("Selecting or creating a body focuses it automatically. Newly created procedural bodies use a neutral staged sphere until their own generator data is applied, so they never inherit another planet's heightmap. Stars use their photosphere/corona preview immediately.")


func _request_active_body_focus() -> void:
	var body: Resource = _session.active_body()
	if body == null:
		return
	body_focus_requested.emit(String(body.get(&"body_id")))
