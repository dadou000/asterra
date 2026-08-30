extends "res://scripts/world_authoring/world_authoring_runtime_host_phase24.gd"
## Phase 25 host: installs the live shader-source editor while retaining Phase 24's
## persistent root-detail / multi-body preview ownership rules.

const LIVE_SHADER_EDITOR_SCRIPT := preload(
	"res://scripts/world_authoring/world_authoring_editor_live_phase26.gd")


func _open_live_editor(player: Node) -> void:
	_opened = true
	_preview_player = player
	_set_existing_ui_visible(false)
	player.set("input_enabled", false)
	if player.has_method("set_mouse_captured"):
		player.call("set_mouse_captured", false)
	_layer = CanvasLayer.new()
	_layer.name = "PlanetStudioLiveLayer"
	_layer.layer = 90
	add_child(_layer)
	var live_editor: Control = LIVE_SHADER_EDITOR_SCRIPT.new()
	live_editor.name = "PlanetStudioLive"
	live_editor.call("bind_world", _main)
	live_editor.connect("runtime_apply_requested", Callable(self, "_on_runtime_apply_requested"))
	if live_editor.has_signal("body_focus_requested"):
		live_editor.connect("body_focus_requested", Callable(self, "_on_body_focus_requested"))
	_layer.add_child(live_editor)
	_editor = live_editor

	var session_value: Variant = live_editor.get("_session")
	if session_value is WorldAuthoringSession:
		var session: WorldAuthoringSession = session_value as WorldAuthoringSession
		_authoring_session = session
		if session.applied_system != null:
			_runtime_applied_snapshot = session.applied_system.duplicate(true)
			_detailed_runtime_body_id = String(session.applied_system.get(&"active_body_id"))
		if not session.changed.is_connected(_on_authoring_session_changed):
			session.changed.connect(_on_authoring_session_changed)

		var celestial_preview: Node3D = CELESTIAL_PREVIEW_SCRIPT.new() as Node3D
		if celestial_preview != null:
			celestial_preview.name = "PlanetStudioCelestialSystemPreview"
			_main.add_child(celestial_preview)
			_celestial_preview = celestial_preview

		var biome_preview: Node = BIOME_PREVIEW_SCRIPT.new()
		biome_preview.name = "PlanetStudioBiomePreview"
		biome_preview.call("bind", session, _main)
		add_child(biome_preview)
		_biome_preview = biome_preview
		if live_editor.has_signal("biome_preview_stroke_added"):
			live_editor.connect("biome_preview_stroke_added",
				Callable(biome_preview, "append_transient_stroke"))
		if live_editor.has_signal("biome_preview_transient_cleared"):
			live_editor.connect("biome_preview_transient_cleared",
				Callable(biome_preview, "clear_transient_strokes"))

		var authored_water_script: Script = _resolve_authored_water_runtime_script()
		if authored_water_script != null:
			var authored_water: Node = authored_water_script.new() as Node
			if authored_water != null:
				authored_water.name = "PlanetStudioAuthoredWaterRuntime"
				authored_water.call("bind", session, _main)
				add_child(authored_water)
				authored_water.add_to_group(&"authored_water_query")
				_authored_water_runtime = authored_water
				if live_editor.has_signal("water_preview_changed"):
					live_editor.connect("water_preview_changed", Callable(authored_water, "mark_dirty"))
		else:
			_set_editor_status("Authored-water runtime scripts are missing from this checkout; terrain authoring remains available.")

		if Planet.has_signal("world_ready") and not Planet.world_ready.is_connected(_on_planet_world_ready):
			Planet.world_ready.connect(_on_planet_world_ready)
		_schedule_active_body_preview(true)
