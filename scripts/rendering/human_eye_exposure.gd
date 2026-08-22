class_name HumanEyeExposure
extends Node
## Asymmetric, two-stage exposure adaptation for the planetary world.
##
## Godot performs the actual full-frame HDR luminance reduction on the GPU. This
## controller supplies physiology-inspired timing and explicit scene-linear
## metering limits. Bright adaptation is fast; dark adaptation first follows the
## cone response, then gradually unlocks the deeper rod threshold.

# The renderer's AgX white point is 12.0, so values at and above this level do
# not drive the exposure farther down. The two darkness floors span six stops
# for cone vision and nine stops for fully adapted rod vision.
const HIGHLIGHT_CLIP_LUMINANCE := 12.0
const CONE_DARKNESS_LUMINANCE := 0.015625      # 1 / 64
const ROD_DARKNESS_LUMINANCE := 0.001953125    # 1 / 512
const MIDDLE_GREY := 0.18

# CameraAttributesPractical converts ISO sensitivity to renderer luminance as
# sensitivity * ((12.5 / 100) / ISO). At the fixed ISO 100 baseline this is
# exactly sensitivity / 800.
const BASE_ISO := 100.0
const SENSITIVITY_PER_LUMINANCE := 800.0

# Research-calibrated response constants.
const LIGHT_ADAPTATION_RATE := 5.0             # 0.20 s time constant
const CONE_DARK_BASE := 0.996787282
const ROD_DARK_BASE := 0.999637396
const ADAPTATION_SAMPLE_SECONDS := 0.28
const ROD_ONSET_SECONDS := 420.0
const NIGHT_RESET_LUMINANCE := 0.05
const DIRECTION_HYSTERESIS_STOPS := 0.10

var observer: AsterraPlayer
var attributes: CameraAttributesPractical

var _adapted_luminance := MIDDLE_GREY
var _dark_seconds := 0.0
var _initialized := false

var _cone_dark_rate := -log(CONE_DARK_BASE) / ADAPTATION_SAMPLE_SECONDS
var _rod_dark_rate := -log(ROD_DARK_BASE) / ADAPTATION_SAMPLE_SECONDS


func configure(world_environment: WorldEnvironment) -> void:
	attributes = CameraAttributesPractical.new()
	attributes.exposure_sensitivity = BASE_ISO
	attributes.exposure_multiplier = 1.0
	attributes.auto_exposure_scale = MIDDLE_GREY
	attributes.auto_exposure_min_sensitivity = _to_sensitivity(CONE_DARKNESS_LUMINANCE)
	attributes.auto_exposure_max_sensitivity = _to_sensitivity(HIGHLIGHT_CLIP_LUMINANCE)
	attributes.auto_exposure_speed = LIGHT_ADAPTATION_RATE
	attributes.auto_exposure_enabled = true
	world_environment.camera_attributes = attributes


func observe(p_observer: AsterraPlayer) -> void:
	observer = p_observer
	_initialized = false
	_dark_seconds = 0.0


func _process(delta: float) -> void:
	if attributes == null or observer == null or not is_instance_valid(observer):
		return

	var metered := _metered_luminance_proxy()
	if not _initialized:
		_adapted_luminance = metered
		_initialized = true

	# Sustained low light unlocks rod sensitivity only after the cone phase. Any
	# meaningful return to daylight rapidly bleaches that accumulated adaptation.
	if metered <= NIGHT_RESET_LUMINANCE:
		_dark_seconds += delta
	else:
		_dark_seconds = 0.0

	var hysteresis := pow(2.0, DIRECTION_HYSTERESIS_STOPS)
	var becoming_brighter := metered > _adapted_luminance * hysteresis
	var rate := LIGHT_ADAPTATION_RATE if becoming_brighter else _dark_adaptation_rate()
	var weight := 1.0 - exp(-rate * delta)
	_adapted_luminance = exp(lerpf(
		log(maxf(_adapted_luminance, ROD_DARKNESS_LUMINANCE)),
		log(maxf(metered, ROD_DARKNESS_LUMINANCE)),
		weight
	))

	if not is_equal_approx(attributes.auto_exposure_speed, rate):
		attributes.auto_exposure_speed = rate
	var min_sensitivity := _to_sensitivity(_current_darkness_floor())
	if not is_equal_approx(attributes.auto_exposure_min_sensitivity, min_sensitivity):
		attributes.auto_exposure_min_sensitivity = min_sensitivity


func _dark_adaptation_rate() -> float:
	return _rod_dark_rate if _dark_seconds >= ROD_ONSET_SECONDS else _cone_dark_rate


func _current_darkness_floor() -> float:
	if _dark_seconds <= ROD_ONSET_SECONDS:
		return CONE_DARKNESS_LUMINANCE
	var rod_time := _dark_seconds - ROD_ONSET_SECONDS
	var rod_progress := 1.0 - exp(-_rod_dark_rate * rod_time)
	# Sensitivity changes logarithmically, so interpolate the threshold in stops.
	return exp(lerpf(
		log(CONE_DARKNESS_LUMINANCE),
		log(ROD_DARKNESS_LUMINANCE),
		rod_progress
	))


func _metered_luminance_proxy() -> float:
	# Godot meters the real rendered HDR frame. This analytic proxy is used only
	# to choose the correct directional time constant, because the renderer offers
	# one speed rather than separate light-to-dark and dark-to-light rates.
	var up := observer.up_dir()
	var view := observer.view_dir().normalized()
	var sun_dir := Frames.helion_dir.normalized()
	var solar_height := up.dot(sun_dir)
	var daylight := _smoothstep(-0.08, 0.20, solar_height)
	var twilight := _smoothstep(-0.22, -0.015, solar_height) \
		* (1.0 - _smoothstep(-0.015, 0.16, solar_height))
	var sky_fraction := _smoothstep(-0.20, 0.45, view.dot(up))
	var scene_luminance := ROD_DARKNESS_LUMINANCE
	scene_luminance += daylight * lerpf(0.12, 0.42, sky_fraction)
	scene_luminance += twilight * 0.025

	# Looking directly toward the solar disc is a genuine bright transition and
	# must contract exposure immediately even if the local surface is near dusk.
	var sun_alignment := view.dot(sun_dir)
	var solar_view := _smoothstep(cos(0.035), cos(0.006), sun_alignment)
	scene_luminance += solar_view * daylight * HIGHLIGHT_CLIP_LUMINANCE
	return clampf(scene_luminance, ROD_DARKNESS_LUMINANCE, HIGHLIGHT_CLIP_LUMINANCE)


func _to_sensitivity(luminance: float) -> float:
	return luminance * SENSITIVITY_PER_LUMINANCE


func _smoothstep(edge0: float, edge1: float, value: float) -> float:
	var t := clampf((value - edge0) / maxf(edge1 - edge0, 1.0e-6), 0.0, 1.0)
	return t * t * (3.0 - 2.0 * t)
