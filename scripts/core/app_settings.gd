extends Node
## Small persistent settings store shared by the launcher and runtime debug UI.

signal debug_closest_character_distance_changed(enabled: bool)

const SETTINGS_PATH := "user://settings.cfg"
const SECTION_DEBUG := "debug"
const KEY_CLOSEST_CHARACTER_DISTANCE := "closest_character_distance"

var debug_closest_character_distance: bool = false

func _ready() -> void:
	_load_settings()

func set_debug_closest_character_distance(enabled: bool) -> void:
	if debug_closest_character_distance == enabled:
		return
	debug_closest_character_distance = enabled
	_save_settings()
	debug_closest_character_distance_changed.emit(enabled)

func _load_settings() -> void:
	var config := ConfigFile.new()
	if config.load(SETTINGS_PATH) != OK:
		return
	debug_closest_character_distance = bool(config.get_value(
		SECTION_DEBUG,
		KEY_CLOSEST_CHARACTER_DISTANCE,
		false
	))

func _save_settings() -> void:
	var config := ConfigFile.new()
	# Preserve any future settings already present in the same file.
	config.load(SETTINGS_PATH)
	config.set_value(
		SECTION_DEBUG,
		KEY_CLOSEST_CHARACTER_DISTANCE,
		debug_closest_character_distance
	)
	var err := config.save(SETTINGS_PATH)
	if err != OK:
		push_warning("Could not save Asterra settings: %s" % error_string(err))
