class_name CharacterPresetLibrary
extends RefCounted

## Small, category-specific appearance presets. The payload deliberately keeps
## identity/revision/ownership separate from the settings so these same files
## can later be replicated between players without changing their schema.

const SCHEMA_VERSION := 1
const PRESET_DIR := "user://asterra/presets"
const PRESET_EXTENSION := ".apreset"
const CATEGORIES := ["hair", "beard", "mustache", "brows", "eyes", "skin"]

static func is_valid_category(category: String) -> bool:
	return CATEGORIES.has(category.to_lower())

static func list_presets(category: String) -> Array[Dictionary]:
	var result: Array[Dictionary] = []
	var normalized := category.to_lower()
	if not is_valid_category(normalized):
		return result
	var directory := DirAccess.open(PRESET_DIR)
	if directory == null:
		return result
	var prefix := normalized + "__"
	for file_name in directory.get_files():
		if not file_name.begins_with(prefix) or not file_name.ends_with(PRESET_EXTENSION):
			continue
		var preset := _load_path(PRESET_DIR.path_join(file_name))
		if not preset.is_empty() and str(preset.get("category", "")) == normalized:
			result.append(preset)
	result.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return str(a.get("name", "")).naturalnocasecmp_to(str(b.get("name", ""))) < 0
	)
	return result

static func load_preset(category: String, preset_id: String) -> Dictionary:
	if not is_valid_category(category) or preset_id.is_empty():
		return {}
	return _load_path(_preset_path(category.to_lower(), preset_id))

## Creates a preset, or updates the existing same-category preset with the same
## display name. Updating preserves its stable ID and increments its revision.
static func save_preset(category: String, display_name: String, settings: Dictionary, owner_id: String = "") -> Dictionary:
	var normalized := category.to_lower()
	var clean_name := display_name.strip_edges()
	if not is_valid_category(normalized):
		return {"ok": false, "error": ERR_INVALID_PARAMETER, "message": "Unknown preset category."}
	if clean_name.is_empty():
		return {"ok": false, "error": ERR_INVALID_PARAMETER, "message": "Enter a preset name."}
	if settings.is_empty():
		return {"ok": false, "error": ERR_INVALID_DATA, "message": "There are no settings to save."}

	var existing := _find_by_name(normalized, clean_name)
	var now := int(Time.get_unix_time_from_system())
	var preset_id := str(existing.get("id", ""))
	if preset_id.is_empty():
		preset_id = _new_id()
	var payload := {
		"schema_version": SCHEMA_VERSION,
		"id": preset_id,
		"category": normalized,
		"name": clean_name,
		"settings": settings.duplicate(true),
		"owner_id": owner_id if not owner_id.is_empty() else str(existing.get("owner_id", "")),
		"revision": int(existing.get("revision", 0)) + 1,
		"created_at": int(existing.get("created_at", now)),
		"updated_at": now,
		"sync_state": "local_only"
	}

	var absolute_dir := ProjectSettings.globalize_path(PRESET_DIR)
	var directory_error := DirAccess.make_dir_recursive_absolute(absolute_dir)
	if directory_error != OK:
		return {"ok": false, "error": directory_error, "message": error_string(directory_error)}
	var file := FileAccess.open_compressed(_preset_path(normalized, preset_id), FileAccess.WRITE, FileAccess.COMPRESSION_ZSTD)
	if file == null:
		var open_error := FileAccess.get_open_error()
		return {"ok": false, "error": open_error, "message": error_string(open_error)}
	file.store_var(payload, true)
	file.close()
	return {"ok": true, "error": OK, "preset": payload}

static func _find_by_name(category: String, display_name: String) -> Dictionary:
	for preset in list_presets(category):
		if str(preset.get("name", "")).nocasecmp_to(display_name) == 0:
			return preset
	return {}

static func _preset_path(category: String, preset_id: String) -> String:
	return PRESET_DIR.path_join("%s__%s%s" % [category, preset_id, PRESET_EXTENSION])

static func _new_id() -> String:
	var crypto := Crypto.new()
	var random_bytes := crypto.generate_random_bytes(16)
	if not random_bytes.is_empty():
		return random_bytes.hex_encode()
	return "%x%x" % [Time.get_ticks_usec(), randi()]

static func _load_path(path: String) -> Dictionary:
	var file := FileAccess.open_compressed(path, FileAccess.READ, FileAccess.COMPRESSION_ZSTD)
	if file == null:
		return {}
	var value = file.get_var(true)
	file.close()
	if not (value is Dictionary):
		return {}
	var preset: Dictionary = value
	if int(preset.get("schema_version", 0)) != SCHEMA_VERSION:
		return {}
	if not is_valid_category(str(preset.get("category", ""))):
		return {}
	if str(preset.get("id", "")).is_empty() or str(preset.get("name", "")).is_empty():
		return {}
	if not (preset.get("settings", null) is Dictionary):
		return {}
	return preset
