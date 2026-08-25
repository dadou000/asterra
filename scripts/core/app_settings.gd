extends Node
## Small persistent settings store shared by the launcher and runtime debug UI.

signal debug_closest_character_distance_changed(enabled: bool)
signal debug_brow_lod_controls_changed(enabled: bool)
signal debug_forced_brow_lod_changed(lod: int)
signal graphics_quality_changed(preset: int)
signal advanced_graphics_changed

const SETTINGS_PATH := "user://settings.cfg"
const SECTION_DEBUG := "debug"
const KEY_CLOSEST_CHARACTER_DISTANCE := "closest_character_distance"
const KEY_BROW_LOD_CONTROLS := "brow_lod_controls"
const KEY_FORCED_BROW_LOD := "forced_brow_lod"
const SECTION_GRAPHICS := "graphics"
const KEY_GRAPHICS_QUALITY := "quality_preset"
const KEY_GRAPHICS_ADVANCED := "advanced_enabled"
const KEY_RENDER_SCALE := "render_scale"
const KEY_SDFGI_ENABLED := "sdfgi_enabled"
const KEY_SDFGI_CASCADES := "sdfgi_cascades"
const KEY_SDFGI_CELL_SIZE := "sdfgi_cell_size"
const KEY_SSAO_ENABLED := "ssao_enabled"
const KEY_SSIL_ENABLED := "ssil_enabled"
const KEY_SSR_ENABLED := "ssr_enabled"
const KEY_GLOW_ENABLED := "glow_enabled"
const KEY_SHADOW_SPLITS := "shadow_splits"
const KEY_CLOUD_QUALITY := "cloud_quality"

## -1 = automatic distance-based selection, 0/1/2 = force that brow LOD.
var debug_closest_character_distance: bool = false
var debug_brow_lod_controls: bool = false
var debug_forced_brow_lod: int = -1
var graphics_quality: int = GraphicsQuality.DEFAULT_PRESET
var graphics_advanced_enabled := false
var advanced_render_scale := 0.77
var advanced_sdfgi_enabled := true
var advanced_sdfgi_cascades := 4
var advanced_sdfgi_cell_size := 0.65
var advanced_ssao_enabled := true
var advanced_ssil_enabled := true
var advanced_ssr_enabled := true
var advanced_glow_enabled := true
var advanced_shadow_splits := 4
var advanced_cloud_quality: int = GraphicsQuality.Preset.HIGH

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
	var changed := graphics_quality != sanitized or graphics_advanced_enabled
	if not changed:
		return
	graphics_quality = sanitized
	graphics_advanced_enabled = false
	_save_settings()
	graphics_quality_changed.emit(sanitized)
	advanced_graphics_changed.emit()

func set_graphics_advanced_enabled(enabled: bool) -> void:
	if graphics_advanced_enabled == enabled:
		return
	graphics_advanced_enabled = enabled
	_save_settings()
	advanced_graphics_changed.emit()

func set_advanced_graphics_value(key: String, value: Variant) -> void:
	match key:
		KEY_RENDER_SCALE:
			advanced_render_scale = clampf(float(value), 0.50, 1.00)
		KEY_SDFGI_ENABLED:
			advanced_sdfgi_enabled = bool(value)
		KEY_SDFGI_CASCADES:
			advanced_sdfgi_cascades = clampi(int(value), 2, 8)
		KEY_SDFGI_CELL_SIZE:
			advanced_sdfgi_cell_size = clampf(float(value), 0.25, 2.00)
		KEY_SSAO_ENABLED:
			advanced_ssao_enabled = bool(value)
		KEY_SSIL_ENABLED:
			advanced_ssil_enabled = bool(value)
		KEY_SSR_ENABLED:
			advanced_ssr_enabled = bool(value)
		KEY_GLOW_ENABLED:
			advanced_glow_enabled = bool(value)
		KEY_SHADOW_SPLITS:
			var splits := int(value)
			advanced_shadow_splits = splits if splits in [0, 1, 2, 4] else 4
		KEY_CLOUD_QUALITY:
			advanced_cloud_quality = GraphicsQuality.sanitize(int(value))
		_:
			return
	_save_settings()
	advanced_graphics_changed.emit()

func advanced_graphics() -> Dictionary:
	return {
		"render_scale": advanced_render_scale,
		"sdfgi_enabled": advanced_sdfgi_enabled,
		"sdfgi_cascades": advanced_sdfgi_cascades,
		"sdfgi_cell_size": advanced_sdfgi_cell_size,
		"ssao_enabled": advanced_ssao_enabled,
		"ssil_enabled": advanced_ssil_enabled,
		"ssr_enabled": advanced_ssr_enabled,
		"glow_enabled": advanced_glow_enabled,
		"shadow_splits": advanced_shadow_splits,
		"cloud_quality": advanced_cloud_quality,
	}

func effective_cloud_quality() -> int:
	return advanced_cloud_quality if graphics_advanced_enabled else graphics_quality

func apply_viewport(viewport: Viewport) -> void:
	GraphicsQuality.configure_viewport(viewport, graphics_quality)
	if graphics_advanced_enabled:
		GraphicsQuality.apply_advanced_viewport(viewport, advanced_graphics())

func apply_world_environment(environment: Environment) -> void:
	GraphicsQuality.configure_world_environment(environment, graphics_quality)
	if graphics_advanced_enabled:
		GraphicsQuality.apply_advanced_world_environment(environment, advanced_graphics())

func apply_sun(light: DirectionalLight3D, studio := false) -> void:
	GraphicsQuality.configure_sun(light, graphics_quality, studio)
	if graphics_advanced_enabled:
		GraphicsQuality.apply_advanced_sun(light, advanced_graphics(), studio)

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
	var has_advanced_values := config.has_section_key(SECTION_GRAPHICS, KEY_RENDER_SCALE)
	if not has_advanced_values:
		_seed_advanced_from_preset(graphics_quality)
	graphics_advanced_enabled = bool(config.get_value(
		SECTION_GRAPHICS, KEY_GRAPHICS_ADVANCED, false))
	advanced_render_scale = clampf(float(config.get_value(
		SECTION_GRAPHICS, KEY_RENDER_SCALE, advanced_render_scale)), 0.50, 1.00)
	advanced_sdfgi_enabled = bool(config.get_value(
		SECTION_GRAPHICS, KEY_SDFGI_ENABLED, advanced_sdfgi_enabled))
	advanced_sdfgi_cascades = clampi(int(config.get_value(
		SECTION_GRAPHICS, KEY_SDFGI_CASCADES, advanced_sdfgi_cascades)), 2, 8)
	advanced_sdfgi_cell_size = clampf(float(config.get_value(
		SECTION_GRAPHICS, KEY_SDFGI_CELL_SIZE, advanced_sdfgi_cell_size)), 0.25, 2.00)
	advanced_ssao_enabled = bool(config.get_value(
		SECTION_GRAPHICS, KEY_SSAO_ENABLED, advanced_ssao_enabled))
	advanced_ssil_enabled = bool(config.get_value(
		SECTION_GRAPHICS, KEY_SSIL_ENABLED, advanced_ssil_enabled))
	advanced_ssr_enabled = bool(config.get_value(
		SECTION_GRAPHICS, KEY_SSR_ENABLED, advanced_ssr_enabled))
	advanced_glow_enabled = bool(config.get_value(
		SECTION_GRAPHICS, KEY_GLOW_ENABLED, advanced_glow_enabled))
	advanced_shadow_splits = int(config.get_value(
		SECTION_GRAPHICS, KEY_SHADOW_SPLITS, advanced_shadow_splits))
	if advanced_shadow_splits not in [0, 1, 2, 4]:
		advanced_shadow_splits = 4
	advanced_cloud_quality = GraphicsQuality.sanitize(int(config.get_value(
		SECTION_GRAPHICS, KEY_CLOUD_QUALITY, advanced_cloud_quality)))

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
	config.set_value(SECTION_GRAPHICS, KEY_GRAPHICS_ADVANCED, graphics_advanced_enabled)
	config.set_value(SECTION_GRAPHICS, KEY_RENDER_SCALE, advanced_render_scale)
	config.set_value(SECTION_GRAPHICS, KEY_SDFGI_ENABLED, advanced_sdfgi_enabled)
	config.set_value(SECTION_GRAPHICS, KEY_SDFGI_CASCADES, advanced_sdfgi_cascades)
	config.set_value(SECTION_GRAPHICS, KEY_SDFGI_CELL_SIZE, advanced_sdfgi_cell_size)
	config.set_value(SECTION_GRAPHICS, KEY_SSAO_ENABLED, advanced_ssao_enabled)
	config.set_value(SECTION_GRAPHICS, KEY_SSIL_ENABLED, advanced_ssil_enabled)
	config.set_value(SECTION_GRAPHICS, KEY_SSR_ENABLED, advanced_ssr_enabled)
	config.set_value(SECTION_GRAPHICS, KEY_GLOW_ENABLED, advanced_glow_enabled)
	config.set_value(SECTION_GRAPHICS, KEY_SHADOW_SPLITS, advanced_shadow_splits)
	config.set_value(SECTION_GRAPHICS, KEY_CLOUD_QUALITY, advanced_cloud_quality)
	var err := config.save(SETTINGS_PATH)
	if err != OK:
		push_warning("Could not save Asterra settings: %s" % error_string(err))

func _seed_advanced_from_preset(preset: int) -> void:
	var defaults := GraphicsQuality.advanced_defaults(preset)
	advanced_render_scale = float(defaults["render_scale"])
	advanced_sdfgi_enabled = bool(defaults["sdfgi_enabled"])
	advanced_sdfgi_cascades = int(defaults["sdfgi_cascades"])
	advanced_sdfgi_cell_size = float(defaults["sdfgi_cell_size"])
	advanced_ssao_enabled = bool(defaults["ssao_enabled"])
	advanced_ssil_enabled = bool(defaults["ssil_enabled"])
	advanced_ssr_enabled = bool(defaults["ssr_enabled"])
	advanced_glow_enabled = bool(defaults["glow_enabled"])
	advanced_shadow_splits = int(defaults["shadow_splits"])
	advanced_cloud_quality = int(defaults["cloud_quality"])
