extends Node
## Small persistent settings store shared by the launcher and runtime debug UI.

signal debug_closest_character_distance_changed(enabled: bool)
signal debug_brow_lod_controls_changed(enabled: bool)
signal debug_forced_brow_lod_changed(lod: int)
signal graphics_quality_changed(preset: int)

const SETTINGS_PATH := "user://settings.cfg"
const SECTION_DEBUG := "debug"
const KEY_CLOSEST_CHARACTER_DISTANCE := "closest_character_distance"
const KEY_BROW_LOD_CONTROLS := "brow_lod_controls"
const KEY_FORCED_BROW_LOD := "forced_brow_lod"
const SECTION_GRAPHICS := "graphics"
const KEY_GRAPHICS_QUALITY := "quality_preset"

## -1 = automatic distance-based selection, 0/1/2 = force that brow LOD.
var debug_closest_character_distance: bool = false
var debug_brow_lod_controls: bool = false
var debug_forced_brow_lod: int = -1
var graphics_quality: int = GraphicsQuality.DEFAULT_PRESET

func _ready() -> void:
	_load_settings()

func set_debug_closest_character_distance(enabled: bool) -> void:
	if debug_closest_character_distance == enabled:
		return
	debug_closest_character_distance = enabled
	_save_settings()
	debug_closest_character_distance_changed.emit(enabled)

func set_debug_brow_lod_controls(enabled: bool) -> void:
	if debug_brow_lod_controls == enabled:
		return
	debug_brow_lod_controls = enabled
	# Turning the debug mode off always returns the groom to normal automatic LOD.
	if not enabled and debug_forced_brow_lod != -1:
		debug_forced_brow_lod = -1
		debug_forced_brow_lod_changed.emit(-1)
	_save_settings()
	debug_brow_lod_controls_changed.emit(enabled)

func set_debug_forced_brow_lod(lod: int) -> void:
	var sanitized := clampi(lod, -1, 2)
	# A forced level only has an effect while brow LOD debugging is enabled.
	if not debug_brow_lod_controls:
		sanitized = -1
	if debug_forced_brow_lod == sanitized:
		return
	debug_forced_brow_lod = sanitized
	_save_settings()
	debug_forced_brow_lod_changed.emit(sanitized)

func set_graphics_quality(preset: int) -> void:
	var sanitized := GraphicsQuality.sanitize(preset)
	if graphics_quality == sanitized:
		return
	graphics_quality = sanitized
	_save_settings()
	graphics_quality_changed.emit(sanitized)

func _load_settings() -> void:
	var config := ConfigFile.new()
	if config.load(SETTINGS_PATH) != OK:
		return
	debug_closest_character_distance = bool(config.get_value(
		SECTION_DEBUG,
		KEY_CLOSEST_CHARACTER_DISTANCE,
		false
	))
	debug_brow_lod_controls = bool(config.get_value(
		SECTION_DEBUG,
		KEY_BROW_LOD_CONTROLS,
		false
	))
	debug_forced_brow_lod = clampi(int(config.get_value(
		SECTION_DEBUG,
		KEY_FORCED_BROW_LOD,
		-1
	)), -1, 2)
	if not debug_brow_lod_controls:
		debug_forced_brow_lod = -1
	graphics_quality = GraphicsQuality.sanitize(int(config.get_value(
		SECTION_GRAPHICS,
		KEY_GRAPHICS_QUALITY,
		GraphicsQuality.DEFAULT_PRESET
	)))

func _save_settings() -> void:
	var config := ConfigFile.new()
	# Preserve any future settings already present in the same file.
	config.load(SETTINGS_PATH)
	config.set_value(
		SECTION_DEBUG,
		KEY_CLOSEST_CHARACTER_DISTANCE,
		debug_closest_character_distance
	)
	config.set_value(
		SECTION_DEBUG,
		KEY_BROW_LOD_CONTROLS,
		debug_brow_lod_controls
	)
	config.set_value(
		SECTION_DEBUG,
		KEY_FORCED_BROW_LOD,
		debug_forced_brow_lod
	)
	config.set_value(SECTION_GRAPHICS, KEY_GRAPHICS_QUALITY, graphics_quality)
	var err := config.save(SETTINGS_PATH)
	if err != OK:
		push_warning("Could not save Asterra settings: %s" % error_string(err))
