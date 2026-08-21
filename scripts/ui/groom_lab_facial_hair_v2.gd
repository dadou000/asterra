extends "res://scripts/ui/groom_lab_facial_hair.gd"

const NaturalFacialHairGenerator = preload("res://scripts/character/natural_facial_hair_groom.gd")

func _late_setup() -> void:
	await get_tree().process_frame
	var root: Node = get_parent()
	_character = root.get_node_or_null("AsterraHuman") as Node3D
	if _character == null:
		await get_tree().process_frame
		_character = root.get_node_or_null("AsterraHuman") as Node3D
	if _character == null:
		push_warning("Groom lab could not find AsterraHuman")
		return

	_collect_meshes(_character)
	_discover_lashes()
	_setup_timers()
	var bounds: AABB = _world_bounds()
	var height: float = maxf(bounds.size.y, 0.5)
	var bottom: float = bounds.position.y
	_ensure_advanced_brow_defaults()

	# New neutral facial-hair baseline. These values are deliberately short and
	# dense; the generator now provides the anatomical placement instead of using
	# long sparse hairs to make the regions visible.
	_mustache_settings["density"] = 0.86
	_mustache_settings["width"] = 0.82
	_mustache_settings["thickness"] = 0.72
	_mustache_settings["length"] = 0.0055
	_mustache_settings["strand_width"] = 0.00030
	_mustache_settings["middle_gap"] = 0.008
	_mustache_settings["droop"] = 0.20
	_mustache_settings["height_offset"] = 0.0
	_mustache_settings["forward_offset"] = 0.00055
	_mustache_settings["messiness"] = 0.14

	_beard_settings["density"] = 0.86
	_beard_settings["coverage"] = 0.42
	_beard_settings["fullness"] = 0.88
	_beard_settings["length"] = 0.0040
	_beard_settings["chin_length"] = 0.0055
	_beard_settings["strand_width"] = 0.00028
	_beard_settings["height_offset"] = 0.0
	_beard_settings["forward_offset"] = 0.00055
	_beard_settings["messiness"] = 0.14

	var default_facial_color: Color = Color(_hair_settings["color"]).darkened(0.10)
	_mustache_settings["color"] = default_facial_color
	_beard_settings["color"] = default_facial_color

	_groom = NaturalFacialHairGenerator.new()
	_groom.name = "ProceduralGroomGenerator"
	add_child(_groom)
	_groom.configure(_character, _meshes, bottom, height, _front_sign)
	_groom.apply_brows(_brow_settings)
	_groom.apply_facial_hair(_mustache_settings, _beard_settings)
	_setup_ui()
	_apply_lash_variant(1 if _available_lash_count() > 0 else 0)
	_refresh_status()
