class_name WorldAuthoringLiveEditorPhase20
extends "res://scripts/world_authoring/world_authoring_editor_live_phase19.gd"
## Phase 20: explicit terrain backend selection and first-class star authoring.
##
## Terrain backend is a body-owned choice:
## - Procedural: existing generated height/material/biome pipeline.
## - Blank: analytic sphere, no generated heightmap/material/biome map. Surface
##   shape is shader-authored and custom painted biome layers remain available.
## Persistent sculpt data is retained but unavailable while Blank is active.

const TERRAIN_MODE_PROFILE_SCRIPT := preload(
	"res://scripts/world_authoring/model/terrain_authoring_profile.gd")
const STAR_PROFILE_SCRIPT := preload(
	"res://scripts/world_authoring/model/star_authoring_profile.gd")


func _build_planet_page() -> void:
	var body: Resource = _session.active_body()
	if body != null and int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
		_build_star_page(body)
		return
	super._build_planet_page()
	_build_terrain_backend_selector()


func _build_terrain_page() -> void:
	super._build_terrain_page()
	if not _blank_terrain_active():
		return
	_section("Blank terrain backend")
	_add_note("No heightmap, macro elevation, procedural biome/material map, tectonics, erosion or climate terrain is generated. The base surface is exactly the body radius. Use the actual terrain shader/displacement authoring for shape. Custom biome paint remains enabled. Sculpt/flatten/smooth/thermal tools are intentionally disabled while Blank is active.")


func _build_celestials_page() -> void:
	super._build_celestials_page()
	_section("Stellar bodies")
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	_workspace.add_child(row)
	var add_star := _toolbar_button("Add Star")
	add_star.tooltip_text = "Create a first-class customizable stellar body."
	add_star.pressed.connect(func() -> void:
		_session.create_body("New Star", BODY_SCRIPT.BodyType.STAR)
		_refresh_all()
	)
	row.add_child(add_star)
	_add_note("Stars persist in the same celestial hierarchy as planets and moons. Their photosphere, spectrum, activity, corona and emitted light are independently authorable.")


func _build_terrain_backend_selector() -> void:
	var terrain: Resource = _session.active_terrain_profile()
	if terrain == null:
		return
	_section("Terrain generation backend")
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)
	_workspace.add_child(row)
	var label := Label.new()
	label.text = "Surface source"
	label.custom_minimum_size.x = 260.0
	row.add_child(label)
	var selector := OptionButton.new()
	selector.custom_minimum_size.x = 260.0
	selector.add_item("Procedural", TERRAIN_MODE_PROFILE_SCRIPT.GenerationMode.PROCEDURAL)
	selector.add_item("Blank — shader-authored", TERRAIN_MODE_PROFILE_SCRIPT.GenerationMode.BLANK)
	selector.select(1 if int(terrain.get(&"generation_mode")) \
		== TERRAIN_MODE_PROFILE_SCRIPT.GenerationMode.BLANK else 0)
	selector.item_selected.connect(func(index: int) -> void:
		var mode: int = selector.get_item_id(index)
		var scope: int = SESSION_SCRIPT.ApplyScope.FULL_REBUILD \
			if mode == TERRAIN_MODE_PROFILE_SCRIPT.GenerationMode.PROCEDURAL \
			else SESSION_SCRIPT.ApplyScope.CLIPMAP
		_session.stage_set(terrain, &"generation_mode", mode, scope,
			"Change terrain generation backend")
		_refresh_current_category()
	)
	row.add_child(selector)
	if _blank_terrain_active():
		_add_note("BLANK: no generated heightmap and no generated biome/material map. Shape = terrain shader. Biomes = custom paint only. Dormant procedural settings are preserved so switching back is lossless.")
	else:
		_add_note("PROCEDURAL: current deterministic tectonics → erosion → climate → biome terrain pipeline, plus shader/material layers and non-destructive sculpt edits.")


func _build_star_page(body: Resource) -> void:
	_page_title("Star", "First-class stellar body authoring. Values are stored per star and do not invoke the terrestrial terrain generator.")
	var star: Resource = body.get(&"star_profile") as Resource
	if star == null:
		body.call("ensure_children")
		star = body.get(&"star_profile") as Resource
	if star == null:
		_add_note("Star profile could not be created.")
		return

	_section("Identity")
	_add_text_field("Display name", String(body.get(&"display_name")), func(value: String) -> void:
		_session.stage_set(body, &"display_name", value, SESSION_SCRIPT.ApplyScope.HOT, "Rename star")
	)

	_section("Physical star")
	_add_number_field("Radius", float(body.get(&"radius_m")) / 1000.0,
		1.0, 1.0e10, 1000.0, " km", func(value: float) -> void:
			_session.stage_set(body, &"radius_m", value * 1000.0,
				SESSION_SCRIPT.ApplyScope.HOT, "Change stellar radius")
	)
	_add_scientific_text_field("Mass", float(body.get(&"mass_kg")), " kg", func(value: float) -> void:
		_session.stage_set(body, &"mass_kg", maxf(value, 0.0), SESSION_SCRIPT.ApplyScope.HOT, "Change stellar mass")
	)
	_add_scientific_text_field("Gravitational parameter", float(body.get(&"gravitational_parameter_m3_s2")), " m³/s²", func(value: float) -> void:
		_session.stage_set(body, &"gravitational_parameter_m3_s2", maxf(value, 0.0), SESSION_SCRIPT.ApplyScope.HOT, "Change stellar GM")
	)
	_add_number_field("Surface gravity", float(body.get(&"surface_gravity_m_s2")),
		0.0, 100000.0, 0.1, " m/s²", func(value: float) -> void:
			_session.stage_set(body, &"surface_gravity_m_s2", value, SESSION_SCRIPT.ApplyScope.HOT, "Change stellar gravity")
	)
	_add_number_field("Sidereal rotation", float(body.call("hours_per_day")),
		0.001, 1.0e7, 0.1, " h", func(value: float) -> void:
			_session.stage_action("Change stellar rotation", func() -> void:
				body.call("set_hours_per_day", value), SESSION_SCRIPT.ApplyScope.HOT)
	)
	_add_number_field("Axial tilt", float(body.get(&"axial_tilt_deg")),
		-180.0, 180.0, 0.1, "°", func(value: float) -> void:
			_session.stage_set(body, &"axial_tilt_deg", value, SESSION_SCRIPT.ApplyScope.HOT, "Change stellar tilt")
	)

	_section("Spectrum")
	_add_star_spectral_selector(star)
	_add_number_field("Effective temperature", float(star.get(&"effective_temperature_k")),
		500.0, 100000.0, 1.0, " K", func(value: float) -> void:
			_star_set(star, &"effective_temperature_k", value, "Change stellar temperature")
	)
	_add_number_field("Luminosity", float(star.get(&"luminosity_solar")),
		0.0, 1.0e8, 0.001, " L☉", func(value: float) -> void:
			_star_set(star, &"luminosity_solar", value, "Change luminosity")
	)
	_add_number_field("Metallicity", float(star.get(&"metallicity_dex")),
		-8.0, 3.0, 0.01, " dex", func(value: float) -> void:
			_star_set(star, &"metallicity_dex", value, "Change metallicity")
	)
	_add_star_color_field("Photosphere color", star.get(&"photosphere_color") as Color, func(value: Color) -> void:
		_star_set(star, &"photosphere_color", value, "Change photosphere color")
	)
	_add_number_field("Photosphere intensity", float(star.get(&"photosphere_intensity")),
		0.0, 1000.0, 0.01, "×", func(value: float) -> void:
			_star_set(star, &"photosphere_intensity", value, "Change photosphere intensity")
	)
	_add_number_field("Limb darkening", float(star.get(&"limb_darkening")),
		0.0, 1.0, 0.01, "", func(value: float) -> void:
			_star_set(star, &"limb_darkening", value, "Change limb darkening")
	)

	_section("Photosphere dynamics")
	_add_number_field("Granulation scale", float(star.get(&"granulation_scale")),
		0.001, 100.0, 0.01, "×", func(value: float) -> void:
			_star_set(star, &"granulation_scale", value, "Change granulation scale")
	)
	_add_number_field("Granulation contrast", float(star.get(&"granulation_contrast")),
		0.0, 4.0, 0.01, "", func(value: float) -> void:
			_star_set(star, &"granulation_contrast", value, "Change granulation contrast")
	)
	_add_number_field("Granulation speed", float(star.get(&"granulation_speed")),
		0.0, 100.0, 0.01, "×", func(value: float) -> void:
			_star_set(star, &"granulation_speed", value, "Change granulation speed")
	)
	_add_number_field("Sunspot coverage", float(star.get(&"sunspot_coverage")) * 100.0,
		0.0, 100.0, 0.01, "%", func(value: float) -> void:
			_star_set(star, &"sunspot_coverage", value / 100.0, "Change sunspot coverage")
	)
	_add_number_field("Sunspot scale", float(star.get(&"sunspot_scale")),
		0.001, 100.0, 0.01, "×", func(value: float) -> void:
			_star_set(star, &"sunspot_scale", value, "Change sunspot scale")
	)
	_add_number_field("Sunspot contrast", float(star.get(&"sunspot_contrast")),
		0.0, 1.0, 0.01, "", func(value: float) -> void:
			_star_set(star, &"sunspot_contrast", value, "Change sunspot contrast")
	)
	_add_number_field("Facula strength", float(star.get(&"facula_strength")),
		0.0, 20.0, 0.01, "×", func(value: float) -> void:
			_star_set(star, &"facula_strength", value, "Change facula strength")
	)
	_add_number_field("Differential rotation", float(star.get(&"differential_rotation")),
		0.0, 1.0, 0.01, "", func(value: float) -> void:
			_star_set(star, &"differential_rotation", value, "Change differential rotation")
	)

	_section("Magnetic activity")
	_add_number_field("Flare activity", float(star.get(&"flare_activity")),
		0.0, 100.0, 0.01, "×", func(value: float) -> void:
			_star_set(star, &"flare_activity", value, "Change flare activity")
	)
	_add_number_field("Flare frequency", float(star.get(&"flare_frequency")),
		0.0, 1000.0, 0.01, "×", func(value: float) -> void:
			_star_set(star, &"flare_frequency", value, "Change flare frequency")
	)
	_add_number_field("Flare energy", float(star.get(&"flare_energy_scale")),
		0.0, 1000.0, 0.01, "×", func(value: float) -> void:
			_star_set(star, &"flare_energy_scale", value, "Change flare energy")
	)
	_add_number_field("Prominence activity", float(star.get(&"prominence_activity")),
		0.0, 100.0, 0.01, "×", func(value: float) -> void:
			_star_set(star, &"prominence_activity", value, "Change prominence activity")
	)

	_section("Corona + stellar wind")
	_add_star_color_field("Corona color", star.get(&"corona_color") as Color, func(value: Color) -> void:
		_star_set(star, &"corona_color", value, "Change corona color")
	)
	_add_number_field("Corona intensity", float(star.get(&"corona_intensity")),
		0.0, 1000.0, 0.01, "×", func(value: float) -> void:
			_star_set(star, &"corona_intensity", value, "Change corona intensity")
	)
	_add_number_field("Corona extent", float(star.get(&"corona_extent_radii")),
		1.0, 100.0, 0.01, " R★", func(value: float) -> void:
			_star_set(star, &"corona_extent_radii", value, "Change corona extent")
	)
	_add_number_field("Stellar wind", float(star.get(&"solar_wind_strength")),
		0.0, 1000.0, 0.01, "×", func(value: float) -> void:
			_star_set(star, &"solar_wind_strength", value, "Change stellar wind")
	)

	_section("Emitted light")
	_add_star_color_field("Light color", star.get(&"light_color") as Color, func(value: Color) -> void:
		_star_set(star, &"light_color", value, "Change stellar light color")
	)
	_add_number_field("Light energy", float(star.get(&"light_energy")),
		0.0, 100000.0, 0.01, "×", func(value: float) -> void:
			_star_set(star, &"light_energy", value, "Change stellar light energy")
	)
	_add_number_field("Angular light radius", float(star.get(&"angular_light_radius_deg")),
		0.0, 45.0, 0.001, "°", func(value: float) -> void:
			_star_set(star, &"angular_light_radius_deg", value, "Change angular light radius")
	)

	_section("Variability + shader")
	_add_number_field("Variability amplitude", float(star.get(&"variability_amplitude")),
		0.0, 10.0, 0.0001, "", func(value: float) -> void:
			_star_set(star, &"variability_amplitude", value, "Change stellar variability")
	)
	_add_number_field("Variability period", float(star.get(&"variability_period_s")),
		0.0, 1.0e12, 1.0, " s", func(value: float) -> void:
			_star_set(star, &"variability_period_s", value, "Change variability period")
	)
	_add_text_field("Surface shader", String(star.get(&"surface_shader_path")), func(value: String) -> void:
		_star_set(star, &"surface_shader_path", value.strip_edges(), "Change star surface shader")
	)
	_add_note("The stellar profile is persistent now. Rendering a selected star as the active preview body is a separate runtime pass; editing and presets already preserve every value above.")


func _add_star_spectral_selector(star: Resource) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)
	_workspace.add_child(row)
	var label := Label.new()
	label.text = "Spectral class"
	label.custom_minimum_size.x = 260.0
	row.add_child(label)
	var selector := OptionButton.new()
	selector.custom_minimum_size.x = 240.0
	for name: String in ["O", "B", "A", "F", "G", "K", "M", "Custom"]:
		selector.add_item(name)
	selector.select(clampi(int(star.get(&"spectral_class")), 0, 7))
	selector.item_selected.connect(func(index: int) -> void:
		_star_set(star, &"spectral_class", index, "Change spectral class")
	)
	row.add_child(selector)


func _add_star_color_field(label_text: String, value: Color, callback: Callable) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)
	_workspace.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 260.0
	row.add_child(label)
	var picker := ColorPickerButton.new()
	picker.color = value
	picker.custom_minimum_size = Vector2(240.0, 30.0)
	picker.color_changed.connect(func(next_color: Color) -> void: callback.call(next_color))
	row.add_child(picker)


func _add_scientific_text_field(label_text: String, value: float, suffix: String,
		callback: Callable) -> void:
	_add_text_field(label_text, "%g%s" % [value, suffix], func(text: String) -> void:
		var clean := text.strip_edges()
		if not suffix.is_empty() and clean.ends_with(suffix):
			clean = clean.left(clean.length() - suffix.length()).strip_edges()
		if not clean.is_valid_float():
			_set_status("%s requires a numeric value." % label_text)
			return
		callback.call(clean.to_float())
	)


func _star_set(star: Resource, property_name: StringName, value: Variant,
		action_name: String) -> void:
	_session.stage_set(star, property_name, value, SESSION_SCRIPT.ApplyScope.HOT, action_name)


func _blank_terrain_active() -> bool:
	var terrain: Resource = _session.active_terrain_profile()
	return terrain != null and int(terrain.get(&"generation_mode")) \
		== TERRAIN_MODE_PROFILE_SCRIPT.GenerationMode.BLANK


func _reject_blank_sculpt() -> bool:
	if not _blank_terrain_active():
		return false
	_set_status("Blank terrain has no authored heightfield: use the terrain displacement shader for shape. Custom biome paint remains available.")
	return true


func _place_sculpt_stroke(direction: Vector3, continuous: bool, sign_value: float) -> void:
	if _reject_blank_sculpt():
		return
	super._place_sculpt_stroke(direction, continuous, sign_value)


func _place_erase_stroke(direction: Vector3, continuous: bool) -> void:
	if _reject_blank_sculpt():
		return
	super._place_erase_stroke(direction, continuous)


func _place_flatten_stroke(direction: Vector3, continuous: bool) -> void:
	if _reject_blank_sculpt():
		return
	super._place_flatten_stroke(direction, continuous)


func _place_smooth_stroke(direction: Vector3, continuous: bool) -> void:
	if _reject_blank_sculpt():
		return
	super._place_smooth_stroke(direction, continuous)


func _place_thermal_stroke(direction: Vector3, continuous: bool) -> void:
	if _reject_blank_sculpt():
		return
	super._place_thermal_stroke(direction, continuous)
